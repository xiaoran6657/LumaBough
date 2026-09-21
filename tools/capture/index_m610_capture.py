"""生成 M6-10 图、二进制语义命令和原生 capture marker 的关联索引。"""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
from pathlib import Path


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_trace(data: bytes) -> list[dict]:
    header = b"miniengine.native-rhi-semantic.v1\n"
    if not data.startswith(header):
        raise ValueError("unexpected semantic trace header")
    cursor = len(header)
    records = []

    def take(separator: bytes) -> bytes:
        nonlocal cursor
        end = data.find(separator, cursor)
        if end < 0:
            raise ValueError(f"truncated command at byte {cursor}")
        result = data[cursor:end]
        cursor = end + len(separator)
        return result

    while cursor < len(data):
        offset = cursor
        length = int(take(b":"))
        operation = data[cursor:cursor + length].decode("ascii")
        cursor += length
        if data[cursor:cursor + 2] != b" r":
            raise ValueError(f"resource list missing at byte {cursor}")
        cursor += 2
        resource_count = int(take(b" "))
        resources = [take(b" ").decode("ascii") for _ in range(resource_count)]
        if data[cursor:cursor + 1] != b"i":
            raise ValueError(f"integer list missing at byte {cursor}")
        cursor += 1
        integer_count = int(take(b" "))
        integers = [int(take(b" ")) for _ in range(integer_count)]
        if data[cursor:cursor + 1] != b"f":
            raise ValueError(f"scalar list missing at byte {cursor}")
        cursor += 1
        scalar_count = int(take(b" "))
        scalars = [int(take(b" ")) for _ in range(scalar_count)]
        if data[cursor:cursor + 1] != b"t":
            raise ValueError(f"text missing at byte {cursor}")
        cursor += 1
        text_length = int(take(b":"))
        text = data[cursor:cursor + text_length]
        cursor += text_length
        if len(text) != text_length or data[cursor:cursor + 1] != b"\n":
            raise ValueError(f"truncated payload at byte {cursor}")
        cursor += 1
        record = {"commandIndex": len(records), "byteOffset": offset, "operation": operation,
                  "resources": resources, "integers": integers, "scalarBits": scalars}
        if operation == "SetPipeline":
            record["pipelineKeySha256"] = digest(text)
        else:
            record["text"] = text.decode("utf-8", errors="strict")
        records.append(record)
    return records


def native_markers(path: Path) -> dict[str, list[dict]]:
    markers = {}
    text = path.read_text(encoding="utf-8-sig")
    pix = text.startswith("Queue ID,")
    for row in csv.DictReader(text.splitlines(), delimiter="," if pix else "\t", skipinitialspace=True):
        row = {key.strip(): (value or "").strip() for key, value in row.items() if key}
        name = row.get("Name", "")
        if not name:
            continue
        if pix:
            event = {"queueEventId": int(row["Queue ID"]), "parent": int(row["Parent"])}
            if row.get("Global ID"):
                event["globalEventId"] = int(row["Global ID"])
        else:
            event = {"eventId": int(row["EID"])}
        markers.setdefault(name, []).append(event)
    return markers


def index(directory: Path, events: Path) -> dict:
    trace_path = directory / "semantic-trace.txt"
    graph_path = directory / "m6-framegraph.json"
    metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
    graph = json.loads(graph_path.read_text(encoding="utf-8"))
    commands = parse_trace(trace_path.read_bytes())
    if len(commands) != metadata["rhiCommandCount"]:
        raise ValueError("command count disagrees with runtime metadata")
    markers = native_markers(events)
    live = {p["name"]: p for p in graph["passes"] if p["live"]}
    stack = []
    scopes = []
    for command in commands:
        if command["operation"] == "BeginLabel":
            name = command["text"]
            entry = {"label": name, "commandBegin": command["commandIndex"],
                     "nativeEvents": markers.get(name, [])}
            if name in live:
                entry["graphPassId"] = live[name]["declaration"]
                entry["executionOrder"] = live[name]["executionOrder"]
            elif stack:
                entry["parentLabel"] = stack[-1]["label"]
            stack.append(entry)
            scopes.append(entry)
        elif command["operation"] == "EndLabel":
            if not stack:
                raise ValueError("unbalanced EndLabel")
            stack.pop()["commandEnd"] = command["commandIndex"]
    if stack:
        raise ValueError("unclosed label scope")
    missing = [scope["label"] for scope in scopes if not scope["nativeEvents"]]
    native_order = [scope["nativeEvents"][0].get("queueEventId", scope["nativeEvents"][0].get("eventId"))
                    for scope in scopes if scope["nativeEvents"]]
    if any(left >= right for left, right in zip(native_order, native_order[1:])):
        missing.append("native marker order differs from semantic command order")
    present_roots = [root for root in graph["roots"] if root["kind"] == "Present"]
    present_events = [event for name, items in markers.items() if name.startswith("Present") for event in items]
    end_frames = [command["commandIndex"] for command in commands if command["operation"] == "EndFrame"]
    if len(present_roots) != 1 or not present_events or len(end_frames) != 1:
        missing.append("Present epilogue")
    transition_refs = [reference for command in commands if command["operation"] == "ApplyTransitions"
                       for reference in command["resources"]]
    if len(transition_refs) != len(graph["transitions"]):
        raise ValueError("logical transitions do not match semantic command resource count")
    semantic_resources = {}
    for transition, reference in zip(graph["transitions"], transition_refs):
        logical = transition["resource"]
        if logical in semantic_resources and semantic_resources[logical] != reference:
            raise ValueError("one logical resource changed semantic handle inside a frame")
        semantic_resources[logical] = reference
    environment_names = (
        ["M4.IBL.Environment", "M4.IBL.Irradiance", "M4.IBL.Prefilter", "M4.IBL.BrdfLut"]
        if metadata["backend"] == "d3d11" else
        ["EnvironmentCube", "IrradianceCube", "PrefilteredCube", "BrdfLut"]
    )
    resource_rows = []
    for resource in graph["resources"]:
        if not resource["live"]:
            continue
        name = resource["name"]
        native_name = "M6." + name if name in {"ShadowMap", "HdrColor", "SceneDepth", "BackBuffer"} else name
        if name.startswith("M6.Mesh.Vertex."):
            native_name = "M6.Mesh." + name.removeprefix("M6.Mesh.Vertex.") + ".VB"
        elif name.startswith("M6.Mesh.Index."):
            native_name = "M6.Mesh." + name.removeprefix("M6.Mesh.Index.") + ".IB"
        elif name.startswith("M6.MaterialTexture."):
            native_name = name.replace("M6.MaterialTexture.", "M6.Texture.", 1)
        elif name.startswith("M6.Environment."):
            native_name = environment_names[int(name.rsplit(".", 1)[1])]
        elif name in {"M6.Skybox.Vertex", "M6.Skybox.Index"}:
            native_name = "M6.SkyboxVertices" if name.endswith("Vertex") else "M6.SkyboxIndices"
        elif "Constants" in name:
            native_name = "frame constants" if metadata["backend"] == "d3d11" else "M5.D3D12.UploadRing"
        elif name == "ScreenshotReadback":
            native_name = "2D Texture" if metadata["backend"] == "d3d11" else "M6.ScreenshotReadback"
        resource_rows.append({"logicalId": resource["resource"], "logicalName": name,
                              "semanticResourceRef": semantic_resources.get(resource["resource"]),
                              "usedByGraphPassIds": [p["declaration"] for p in graph["passes"]
                                                    if p["live"] and any(u["resource"] == resource["resource"] for u in p["uses"])],
                              "physicalSlot": resource["physicalSlot"], "nativeSearchName": native_name,
                              "nativeLocator": ("Screenshot marker 下的 copy 目标；D3D11 为 staging texture，D3D12 为 readback buffer"
                                                if name == "ScreenshotReadback" else
                                                "按所属 draw marker 与 shader register/offset 区分同名 constant buffer 或 upload ring slice"
                                                if "Constants" in name else "按名称与尺寸/format 搜索"),
                              "kind": resource["kind"], "imported": resource["imported"]})
    return {"schemaVersion": 1, "status": "PASS_MARKERS" if not missing else "FAIL",
            "note": "marker 关联通过不替代 pipeline/resource/PDB 内容核查；资源名称为检索入口。",
            "graphHash": metadata["graphHash"], "commandCount": len(commands),
            "artifacts": {str(trace_path): digest(trace_path.read_bytes()), str(graph_path): digest(graph_path.read_bytes()),
                          str(events): digest(events.read_bytes())},
            "presentation": {"graphRoots": present_roots, "commandEndFrame": end_frames, "nativeEvents": present_events},
            "missingNativeMarkers": missing, "scopes": scopes, "resources": resource_rows,
            "commands": commands}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--events", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = index(args.directory, args.events)
    except (OSError, ValueError, KeyError, UnicodeError) as error:
        result = {"status": "BLOCKED", "reason": str(error)}
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("status", "commandCount", "missingNativeMarkers", "reason") if key in result}))
    return 0 if result["status"] == "PASS_MARKERS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
