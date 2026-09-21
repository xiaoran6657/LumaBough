"""独立核对 M6-08 四份 dump；不依赖图形 viewer 或引擎运行时。"""
from __future__ import annotations
import argparse
import heapq
import json
import re
from pathlib import Path


def require(value, message):
    if not value:
        raise ValueError(message)


def load_unique(text):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, f"duplicate JSON key: {key}")
            result[key] = value
        return result
    return json.loads(text, object_pairs_hook=pairs)


def check(texts):
    graph = load_unique(texts["m6-framegraph.json"])
    access = load_unique(texts["m6-access-plan.json"])
    transient = load_unique(texts["m6-transient-plan.json"])
    # 保存的 canonical section 保持原始 float 格式；不可由 Python 重序列化猜测 C++ 字节。
    canonical = '{"statistics":' + texts["m6-framegraph.json"].split('"statistics":', 1)[1]
    value = 14695981039346656037
    for byte in canonical.encode("utf-8"):
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    require(f"{value:016x}" == graph["planHash"], "canonical FNV1a mismatch")
    for other, kind in [(graph, "framegraph"), (access, "access-plan"), (transient, "transient-plan")]:
        require(other["schemaVersion"] == 1 and other["kind"] == kind, "dump schema/kind")
        for key in ["frame", "build", "commit", "planHash"]:
            require(other[key] == graph[key], f"dump metadata mismatch: {key}")
    require(graph["stages"] == [
        "validate_declarations", "create_nodes", "create_dependencies", "find_roots", "cull",
        "topological_sort", "lifetimes", "physical_slots", "access_transitions", "emit_plan"], "ten stages")
    passes, resources, versions, edges = (graph[k] for k in ["passes", "resources", "versions", "edges"])
    require([p["declaration"] for p in passes] == list(range(len(passes))), "pass IDs")
    require([r["resource"] for r in resources] == list(range(len(resources))), "resource IDs")
    version_map = {(v["resource"], v["version"]): v for v in versions}
    require(len(version_map) == len(versions), "duplicate version")
    producers, readers = {}, {key: [] for key in version_map}
    expected_edges = []
    for p in passes:
        for use in p["uses"]:
            key = (use["resource"], use["version"])
            require(key in version_map, "unknown used version")
            if use["write"]:
                require(key not in producers, "multiple producers")
                producers[key] = p["declaration"]
            else:
                readers[key].append(p["declaration"])
    for key, v in version_map.items():
        require(v["producer"] == producers.get(key), "producer disagreement")
        require(v["readers"] == readers[key], "reader disagreement")
        require(v["importedProducer"] == (key[1] == 0 and resources[key[0]]["imported"]), "import v0")
    for p in passes:
        for use in p["uses"]:
            r = use["resource"]
            v = use["previousVersion"] if use["write"] else use["version"]
            source = version_map[(r, v)]
            keep = False
            if use["write"]:
                attachment = next((a for a in p["attachments"] if a["resource"] == r), None)
                keep = attachment["load"] == "Load" if attachment else use["coverage"] == "Preserve"
            if source["producer"] is not None:
                expected_edges.append((source["producer"], p["declaration"], r, v,
                                       "WAW" if use["write"] else "RAW", keep if use["write"] else True))
            if use["write"]:
                for reader in source["readers"]:
                    expected_edges.append((reader, p["declaration"], r, v, "WAR", False))
    edge_keys = ["from", "to", "resource", "version", "hazard", "contributesToOutput"]
    require(sorted(tuple(e[k] for k in edge_keys) for e in edges) == sorted(set(expected_edges)), "RAW/WAR/WAW")
    live, root_resources = set(), set()
    for root in graph["roots"]:
        if root["kind"] == "SideEffect":
            require(bool(root["reason"]) and passes[root["pass"]]["sideEffect"] == root["reason"], "side-effect reason")
            live.add(root["pass"])
        else:
            key = (root["resource"], root["version"])
            require(resources[key[0]]["imported"], "transient output ownership")
            require(key[1] == max(v for r, v in version_map if r == key[0]), "nonfinal output version")
            if root["kind"] == "Present":
                require(resources[key[0]]["finalAccess"] == "Present", "Present access")
            root_resources.add(key[0])
            producer = version_map[key]["producer"]
            if producer is not None:
                live.add(producer)
    changed = True
    while changed:
        before = len(live)
        live.update(e["from"] for e in edges if e["to"] in live and e["contributesToOutput"])
        changed = len(live) != before
    require(live == {p["declaration"] for p in passes if p["live"]}, "reverse root reachability")
    for p in passes:
        require(bool(p["culledReason"]) == (p["declaration"] not in live), "cull reason")
    indegree = {p: 0 for p in live}
    for edge in edges:
        require(edge["live"] == (edge["from"] in live and edge["to"] in live), "edge live flag")
        if edge["live"]:
            indegree[edge["to"]] += 1
    ready = [p for p, degree in indegree.items() if degree == 0]
    heapq.heapify(ready)
    order = []
    while ready:
        p = heapq.heappop(ready)
        order.append(p)
        for edge in edges:
            if edge["live"] and edge["from"] == p:
                indegree[edge["to"]] -= 1
                if indegree[edge["to"]] == 0:
                    heapq.heappush(ready, edge["to"])
    require(len(order) == len(live) and order == graph["executionOrder"], "stable Kahn order")
    live_resources = root_resources | {u["resource"] for p in live for u in passes[p]["uses"]}
    require(live_resources == {r["resource"] for r in resources if r["live"]}, "live resource set")
    live_versions = {(r["resource"], r["version"]) for r in graph["roots"] if r["kind"] != "SideEffect"}
    for p in live:
        for use in passes[p]["uses"]:
            live_versions.add((use["resource"], use["version"]))
            if use["write"]:
                attachment = next((a for a in passes[p]["attachments"] if a["resource"] == use["resource"]), None)
                preserves = attachment["load"] == "Load" if attachment else use["coverage"] == "Preserve"
                if preserves:
                    live_versions.add((use["resource"], use["previousVersion"]))
    require(live_versions == {key for key, version in version_map.items() if version["live"]}, "live version set")
    intervals, transitions = {}, []
    current = {r["resource"]: r["initialAccess"] if r["imported"] else "None" for r in resources}
    allocations = graph["physicalAllocations"]
    require([a["slot"] for a in allocations] == list(range(len(allocations))), "slot IDs")
    require([a["resource"] for a in allocations] == sorted(live_resources), "one slot per live resource")
    slots = {a["resource"]: a["slot"] for a in allocations}
    for resource in resources:
        require(resource["physicalSlot"] == slots.get(resource["resource"]), "virtual/physical mapping")
    for allocation in allocations:
        resource = resources[allocation["resource"]]
        require(allocation["kind"] == resource["kind"] and allocation["imported"] == resource["imported"], "slot ownership")
    for i, p in enumerate(order):
        require(passes[p]["executionOrder"] == i, "pass execution index")
        for u in passes[p]["uses"]:
            r = u["resource"]
            intervals.setdefault(r, [i, i])[1] = i
            transitions.append((p, i, r, slots[r], current[r], u["access"], False))
            current[r] = u["access"]
    for r in sorted(live_resources):
        if resources[r]["imported"]:
            intervals.setdefault(r, [len(order), len(order)])[1] = len(order)
            transitions.append((len(passes), len(order), r, slots[r], current[r], resources[r]["finalAccess"], True))
    for lifetime in graph["lifetimes"]:
        r = lifetime["resource"]
        require([lifetime["firstUse"], lifetime["lastUse"]] == intervals[r], "lifetime")
        require(lifetime["physicalSlot"] == slots[r] and lifetime["imported"] == resources[r]["imported"], "lifetime slot")
    transition_keys = ["pass", "executionOrder", "resource", "physicalSlot", "before", "after", "final"]
    require([tuple(t[k] for k in transition_keys) for t in graph["transitions"]] == transitions, "live access plan")
    require(access["transitions"] == graph["transitions"], "access dump")
    require(transient["lifetimes"] == graph["lifetimes"] and transient["physicalAllocations"] == allocations, "transient dump")
    require(transient["policy"] == "one_slot_per_live_resource_no_pool_or_aliasing", "M6-08 slot policy")
    stats = graph["statistics"]
    expected_stats = dict(declaredPasses=len(passes), livePasses=len(live), culledPasses=len(passes)-len(live),
                          virtualResources=len(resources), liveResources=len(live_resources), resourceVersions=len(versions),
                          dependencyEdges=len(edges), physicalResources=len(allocations),
                          physicalTransients=sum(not a["imported"] for a in allocations), roots=len(graph["roots"]),
                          logicalTransitions=len(transitions))
    require(stats == expected_stats, "statistics")

    dot = texts["m6-framegraph.dot"]
    node_pattern = r"(?m)^  ((?:p|root|side)\d+|r\d+v\d+) \["
    dot_nodes = re.findall(node_pattern, dot)
    # DOT 的 root index 只枚举资源 roots；SideEffect 使用自己的 pass ID。
    resource_roots = [r for r in graph["roots"] if r["kind"] != "SideEffect"]
    expected_nodes = {f"p{i}" for i in range(len(passes))} | {f"r{r}v{v}" for r, v in version_map}
    expected_nodes.update(f"root{i}" for i in range(len(resource_roots)))
    expected_nodes.update(f"side{p['declaration']}" for p in passes if p["sideEffect"])
    require(set(dot_nodes) == expected_nodes and len(dot_nodes) == len(expected_nodes), "DOT nodes")
    dot_edges = re.findall(r"(?m)^  (\w+) -> (\w+)", dot)
    expected_dot_edges = [(f"p{v['producer']}", f"r{v['resource']}v{v['version']}")
                          for v in versions if v["producer"] is not None]
    expected_dot_edges += [(f"r{v['resource']}v{v['version']}", f"p{reader}") for v in versions for reader in v["readers"]]
    expected_dot_edges += [(f"p{e['from']}", f"p{e['to']}") for e in edges]
    expected_dot_edges += [(f"r{r['resource']}v{r['version']}", f"root{i}") for i, r in enumerate(resource_roots)]
    expected_dot_edges += [(f"p{p['declaration']}", f"side{p['declaration']}") for p in passes if p["sideEffect"]]
    require(sorted(dot_edges) == sorted(expected_dot_edges), "DOT producer/read/hazard/root edges")
    for p in passes:
        color = "lightblue" if p["live"] else "lightgray"
        require(re.search(rf'^  p{p["declaration"]} \[.*fillcolor="{color}"', dot, re.M), "DOT pass color")
    for v in versions:
        color = "lightgray" if not v["live"] else "lightgreen" if resources[v["resource"]]["imported"] else "orange"
        require(re.search(rf'^  r{v["resource"]}v{v["version"]} \[.*fillcolor="{color}"', dot, re.M), "DOT version color")
    return {"status": "PASS", "planHash": graph["planHash"], "statistics": stats,
            "dotNodes": len(dot_nodes), "dotEdges": len(dot_edges),
            "canonicalBytes": len(canonical.encode("utf-8")), "metadata": {k: graph[k] for k in ["frame", "build", "commit"]}}


def negative_controls(texts):
    failures = []
    for label, filename, old, new in [
        ("hash", "m6-framegraph.json", '"planHash":"', '"planHash":"bad'),
        ("stage", "m6-framegraph.json", '"validate_declarations"', '"create_nodes"'),
        ("access", "m6-access-plan.json", '"transitions":[', '"transitions":[{} ,'),
        ("dot", "m6-framegraph.dot", "digraph FrameGraph {", "digraph FrameGraph {\n  unknown -> p0;"),
    ]:
        altered = dict(texts)
        require(old in altered[filename], "negative control fixture drift")
        altered[filename] = altered[filename].replace(old, new, 1)
        if label == "stage":
            original_hash = load_unique(altered["m6-framegraph.json"])["planHash"]
            canonical = '{"statistics":' + altered["m6-framegraph.json"].split('"statistics":', 1)[1]
            value = 14695981039346656037
            for byte in canonical.encode("utf-8"):
                value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
            altered["m6-framegraph.json"] = altered["m6-framegraph.json"].replace(original_hash, f"{value:016x}", 1)
            for other in ["m6-access-plan.json", "m6-transient-plan.json"]:
                altered[other] = altered[other].replace(original_hash, f"{value:016x}", 1)
        try:
            check(altered)
        except (ValueError, KeyError, TypeError):
            failures.append(label)
        else:
            raise ValueError(f"negative control accepted: {label}")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    names = ["m6-framegraph.json", "m6-framegraph.dot", "m6-access-plan.json", "m6-transient-plan.json"]
    texts = {name: (args.directory / name).read_text(encoding="utf-8") for name in names}
    report = check(texts)
    report["negativeControlsRejected"] = negative_controls(texts)
    serialized = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized, encoding="utf-8")
    print(serialized, end="")


if __name__ == "__main__":
    main()
