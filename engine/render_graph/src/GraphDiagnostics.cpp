#include "GraphJson.h"
#include "GraphState.h"

namespace MiniEngine::RenderGraph
{
std::string_view ToString(CompileStage stage)
{
    constexpr std::string_view names[] = {
        "validate_declarations", "create_nodes", "create_dependencies", "find_roots",         "cull",
        "topological_sort",      "lifetimes",    "physical_slots",      "access_transitions", "emit_plan"};
    const auto value = static_cast<std::size_t>(stage);
    return value < std::size(names) ? names[value] : "unknown";
}
std::string_view ToString(Hazard hazard)
{
    switch (hazard)
    {
    case Hazard::ReadAfterWrite:
        return "RAW";
    case Hazard::WriteAfterRead:
        return "WAR";
    case Hazard::WriteAfterWrite:
        return "WAW";
    }
    return "unknown";
}
std::string_view ToString(GraphErrorCode code)
{
    constexpr std::string_view names[] = {"invalid_declaration", "stale_handle",   "undefined_content",
                                          "phase_violation",     "cycle",          "invalid_import",
                                          "execution_failure",   "missing_present"};
    const auto value = static_cast<std::size_t>(code);
    return value < std::size(names) ? names[value] : "unknown";
}
std::string ToDiagnosticJson(const GraphDiagnostic& error)
{
    Detail::JsonText out;
    out.Raw("{\"schemaVersion\":1,\"code\":");
    out.String(ToString(error.code));
    out.Raw(",\"phase\":");
    out.Number(static_cast<unsigned>(error.phase));
    out.Raw(",\"stage\":");
    out.String(ToString(error.stage));
    out.Raw(",\"pass\":");
    out.Index(error.pass);
    out.Raw(",\"passName\":");
    out.String(error.passName);
    out.Raw(",\"resource\":");
    out.Index(error.resource);
    out.Raw(",\"resourceName\":");
    out.String(error.resourceName);
    out.Raw(",\"version\":");
    out.Index(error.version);
    out.Raw(",\"message\":");
    out.String(error.message);
    out.Raw(",\"cycle\":[");
    bool first = true;
    for (const auto& edge : error.cycle)
    {
        if (!first)
            out.Raw(",");
        first = false;
        out.Raw("{\"from\":");
        out.Index(edge.from);
        out.Raw(",\"to\":");
        out.Index(edge.to);
        out.Raw(",\"resource\":");
        out.Index(edge.resource);
        out.Raw(",\"version\":");
        out.Index(edge.version);
        out.Raw(",\"hazard\":");
        out.String(ToString(edge.hazard));
        out.Raw("}");
    }
    out.Raw("]}\n");
    return std::move(out.text);
}
} // namespace MiniEngine::RenderGraph
namespace MiniEngine::RenderGraph::Detail
{
GraphDiagnostic GraphState::CaptureDiagnostic(std::exception_ptr exception)
{
    GraphDiagnostic result;
    result.phase = phase;
    result.stage = currentStage;
    result.pass = contextPass;
    result.resource = contextResource;
    result.version = contextVersion;
    try
    {
        std::rethrow_exception(exception);
    }
    catch (const GraphCompileError& error)
    {
        const auto& supplied = error.Diagnostic();
        if (supplied.code != GraphErrorCode::InvalidDeclaration || !supplied.cycle.empty() || supplied.pass != kNoPass)
            result = supplied;
        result.message = error.what();
        if (dynamic_cast<const GraphPhaseError*>(&error))
            result.code = GraphErrorCode::PhaseViolation;
        else if (dynamic_cast<const StaleGraphHandleError*>(&error))
            result.code = GraphErrorCode::StaleHandle;
        else if (dynamic_cast<const UndefinedContentError*>(&error))
            result.code = GraphErrorCode::UndefinedContent;
        else if (dynamic_cast<const GraphCycleError*>(&error))
            result.code = GraphErrorCode::Cycle;
        else if (supplied.code == GraphErrorCode::InvalidDeclaration && phase == GraphPhase::Executing)
            result.code = validatingImports ? GraphErrorCode::InvalidImport : GraphErrorCode::ExecutionFailure;
    }
    catch (const Rhi::RhiException& error)
    {
        result.code = phase == GraphPhase::Executing
                          ? (validatingImports ? GraphErrorCode::InvalidImport : GraphErrorCode::ExecutionFailure)
                          : GraphErrorCode::InvalidDeclaration;
        result.message = error.what();
    }
    catch (const std::exception& error)
    {
        result.code =
            phase == GraphPhase::Executing ? GraphErrorCode::ExecutionFailure : GraphErrorCode::InvalidDeclaration;
        result.message = error.what();
    }
    catch (...)
    {
        result.code = GraphErrorCode::ExecutionFailure;
        result.message = "non-standard graph callback failure";
    }
    if (result.pass < passes.size() && result.passName.empty())
        result.passName = passes[result.pass]->name;
    if (result.resource < resources.size() && result.resourceName.empty())
        result.resourceName = resources[result.resource].name;
    if (result.resourceName.empty() && result.resource == kNoPass)
        result.resourceName = contextResourceName;
    return result;
}
} // namespace MiniEngine::RenderGraph::Detail
