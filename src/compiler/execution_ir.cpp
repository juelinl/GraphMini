#include "compiler/execution_ir.h"
#include <set>
#include <sstream>
#include <stdexcept>

namespace minigraph {
void verify_execution(const ExecutionIR &execution, const PlanIR &plan) {
    verify_representations(plan.logical, execution.domains, execution.representations);
    verify_bitmap_region(plan, execution);
    std::map<int, int> depths;
    std::set<int> graphs;
    for (const auto &level : plan.logical.set_ops)
        for (const auto &op : level)
            depths.emplace(op.id, op.loop_depth());
    for (const auto &level : plan.auxiliary.mg_ops)
        for (const auto &mg : level)
            graphs.insert(mg.id);
    auto require = [](bool valid, const char *message) {
        if (!valid)
            throw std::logic_error(message);
    };
    require(execution.sets.size() == depths.size(), "Execution set coverage mismatch");
    std::map<int, int> order;
    int position = 0;
    for (const auto &level : plan.logical.set_ops)
        for (const auto &op : level)
            order.emplace(op.id, position++);
    for (const auto &entry : execution.sets) {
        const int id = entry.first;
        const auto &op = entry.second;
        require(id == op.id && depths.count(id) && depths.at(id) == op.depth,
                "Invalid execution definition");
        auto reference = [&](SetReference ref) {
            switch (ref.source) {
            case SetSource::Prefix:
                require(depths.count(ref.id) && depths.at(ref.id) <= op.depth && ref.id != id,
                        "Invalid prefix reference");
                require(order.at(ref.id) < order.at(id), "Prefix used before its definition");
                require(execution.sets.at(ref.id).result != SetResult::Count,
                        "Count-only result used as a set");
                break;
            case SetSource::GraphAdjacency:
                require(ref.id >= 0 && ref.id <= op.depth, "Invalid adjacency depth");
                break;
            case SetSource::MiniGraphAdjacency:
                require(graphs.count(ref.id), "Unknown MiniGraph reference");
                break;
            }
        };
        auto vertex = [&](const VertexReference &ref) {
            if (ref.adjacency) {
                require(ref.adjacency->source != SetSource::Prefix, "Prefix has no adjacency owner");
                reference(*ref.adjacency);
            } else
                require(ref.depth >= 0 && ref.depth <= op.depth, "Invalid selected vertex");
        };
        reference(op.input);
        for (const auto &step : op.steps) {
            const bool binary = step.opcode == SetOpcode::Intersect ||
                                step.opcode == SetOpcode::DifferenceExcludingOwner;
            require(step.rhs.has_value() == binary && step.vertex.has_value() != binary,
                    "Invalid set operation operands");
            require(binary || !step.upper_bound, "Unary set operation has an extra bound");
            if (step.rhs)
                reference(*step.rhs);
            if (step.opcode == SetOpcode::DifferenceExcludingOwner)
                require(step.rhs->source != SetSource::Prefix, "Difference needs an adjacency owner");
            if (step.vertex)
                vertex(*step.vertex);
            if (step.upper_bound)
                vertex(*step.upper_bound);
        }
        require(op.result != SetResult::Count || (!op.steps.empty() && !op.guard_empty),
                "Count-only operation requires a kernel and cannot guard a set");
    }
    require(execution.minigraphs.size() == graphs.size(), "MiniGraph coverage mismatch");
    for (const auto &[id, mg] : execution.minigraphs) {
        require(id == mg.id && graphs.count(id), "Invalid MiniGraph definition");
        for (int input : {mg.vertices, mg.intersect, mg.iter})
            require(depths.count(input) && depths.at(input) <= mg.depth &&
                        execution.sets.at(input).result != SetResult::Count,
                    "Invalid MiniGraph build input");
        if (mg.parent)
            require(*mg.parent != id && graphs.count(*mg.parent) &&
                        execution.minigraphs.at(*mg.parent).depth <= mg.depth,
                    "Invalid MiniGraph parent");
        for (const auto &estimate : mg.reuse) {
            require(estimate.uses > 0 && depths.count(estimate.initial_set), "Invalid reuse estimate");
            for (const auto &factor : estimate.factors)
                require(!factor.set_id || depths.count(*factor.set_id), "Invalid reuse factor");
        }
    }
    require(execution.loops.size() == plan.logical.set_ops.size(), "Loop coverage mismatch");
    require(execution.serial_loop_boundary >= 1 && execution.serial_loop_boundary <= plan.logical.p_size,
            "Invalid serial loop boundary");
    for (size_t depth = 0; depth < execution.loops.size(); ++depth) {
        const auto &loop = execution.loops[depth];
        for (int id : loop.captured_sets)
            require(depths.count(id) && depths.at(id) < static_cast<int>(depth) &&
                        execution.sets.at(id).result != SetResult::Count,
                    "Invalid set capture");
        for (int id : loop.captured_minigraphs)
            require(graphs.count(id) && execution.minigraphs.at(id).depth < static_cast<int>(depth),
                    "Invalid MiniGraph capture");
        for (int adj : loop.captured_adjacencies)
            require(adj >= 0 && adj < static_cast<int>(depth), "Invalid adjacency capture");
        require(loop.grain_size > 0, "Invalid task grain size");
    }
    for (const auto &term : execution.iep)
        for (const auto &factor : term.factors) {
            require(!factor.empty(), "Empty IEP factor");
            for (int id : factor)
                require(execution.sets.count(id) && execution.sets.at(id).result != SetResult::Count,
                        "Invalid IEP set reference");
        }
}

std::string dump_execution(const ExecutionIR &execution) {
    std::ostringstream out;
    out << dump_domains(execution.domains, execution.representations);
    out << "bitmap: " << execution.bitmap_reason << '\n';
    if (execution.bitmap_region) {
        const auto &region = *execution.bitmap_region;
        out << "bitmap-region @depth" << region.entry_depth << " anchor=" << region.anchor_depth
            << " rows=universe conversions@depth" << region.conversion_depth << " live-ins:";
        for (int id : region.live_ins) out << " set" << id;
        out << " counts:";
        for (int id : region.count_ops) out << " set" << id;
        out << " local-iterator=set" << region.iterator_set;
        out << '\n';
    }
    auto ref = [&](SetReference r) {
        switch (r.source) {
        case SetSource::Prefix:
            out << "set";
            break;
        case SetSource::GraphAdjacency:
            out << "adj";
            break;
        case SetSource::MiniGraphAdjacency:
            out << "mini";
            break;
        }
        out << r.id;
    };
    auto vertex = [&](const VertexReference &v) {
        if (v.adjacency) {
            out << "owner(";
            ref(*v.adjacency);
            out << ')';
        } else
            out << "vertex" << v.depth;
    };
    for (const auto &[id, op] : execution.sets) {
        out << "set" << id << " @depth" << op.depth << " = ";
        ref(op.input);
        for (const auto &step : op.steps) {
            switch (step.opcode) {
            case SetOpcode::Intersect:
                out << " | intersect ";
                break;
            case SetOpcode::DifferenceExcludingOwner:
                out << " | difference-excluding-owner ";
                break;
            case SetOpcode::Bound:
                out << " | bound ";
                break;
            case SetOpcode::Remove:
                out << " | remove ";
                break;
            }
            if (step.rhs)
                ref(*step.rhs);
            if (step.vertex)
                vertex(*step.vertex);
            if (step.upper_bound) {
                out << " upper=";
                vertex(*step.upper_bound);
            }
        }
        switch (op.result) {
        case SetResult::Materialize:
            out << " -> materialize";
            break;
        case SetResult::Count:
            out << " -> count";
            break;
        case SetResult::MaterializeThenCount:
            out << " -> materialize-then-count";
            break;
        }
        if (op.guard_empty)
            out << " guard-empty";
        out << '\n';
        for (const auto &rule : op.rules)
            out << "  rule: " << rule << '\n';
    }
    for (const auto &[id, mg] : execution.minigraphs) {
        out << "mini" << id << " @depth" << mg.depth << " build(set" << mg.vertices << ", set"
            << mg.intersect << ", set" << mg.iter << ')';
        if (mg.parent)
            out << " parent=mini" << *mg.parent;
        out << " eager=" << mg.eager << " bounded=" << mg.bounded << " parallel=" << mg.parallel << '\n';
        out << "  rule: eager-next-use; otherwise configured pruning policy\n";
        for (const auto &[iter, direct] : mg.direct_indices)
            if (iter >= 0)
                out << "  set" << iter << " indices=" << (direct ? "direct-prefix" : "explicit-map")
                    << '\n';
        for (const auto &estimate : mg.reuse) {
            out << "  reuse: size(set" << estimate.initial_set << ')';
            for (const auto &factor : estimate.factors) {
                if (factor.set_id)
                    out << " * size(set" << *factor.set_id << ')';
                out << " * " << factor.scale;
            }
            out << " * " << estimate.uses << '\n';
        }
    }
    for (size_t depth = 0; depth < execution.loops.size(); ++depth) {
        const auto &loop = execution.loops[depth];
        out << "loop" << depth << " adjacency=" << loop.read_adjacency << " nested=" << loop.spawn_nested
            << " grain=" << loop.grain_size << '\n';
        out << "  captures:";
        for (int id : loop.captured_sets)
            out << " set" << id;
        for (int id : loop.captured_minigraphs)
            out << " mini" << id;
        for (int id : loop.captured_adjacencies)
            out << " adj" << id;
        out << '\n';
        if (loop.spawn_nested && loop.runtime_threshold)
            out << "  rule: degree-threshold " << loop.threshold_factor << " * " << loop.average_degree
                << (loop.cap_threshold ? " capped-at-100" : "") << '\n';
    }
    for (const auto &term : execution.iep) {
        out << "iep " << term.coefficient;
        for (const auto &factor : term.factors) {
            out << " * cardinality(";
            for (size_t i = 0; i < factor.size(); ++i) {
                if (i)
                    out << " & ";
                out << "set" << factor[i];
            }
            out << ')';
        }
        out << '\n';
    }
    return out.str();
}
} // namespace minigraph
