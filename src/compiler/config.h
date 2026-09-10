//
// Created by ubuntu on 1/2/23.
//

#pragma once

namespace minigraph {
//---------- Basic Types ----------
    enum AdjMatType {
       VertexInduced = 0, EdgeInduced = 1, EdgeInducedIEP = 2,
    };

    enum class SchedulerType {
        GraphPi = 0,
        GraphMini = 1,
        GraphZero = 2,
        Outgoing = 3, // Experimental: outgoing profile, then canonicality weights.
        BitmapBalanced = 4, // Experimental: bitmap opportunity within a top-eight shortlist.
        IepFirst = 5, // Experimental: supported IEP width, outgoing profile, canonicality.
    };

    enum class PruningType {
        None = 0, // not using minigraph
        Static = 1, // use iff not introducing any redundant set operation
        Eager = 2, // use for all potential cases
        Online = 3, // use iff pruned adj will be used once
        CostModel = 4, // lazy + cost model
    };

    enum class ParallelType {
        OpenMP = 0,
        TbbTop = 1, // only parallel at top level
        Nested = 2, // aggressively nested
        NestedRt = 3, // decide whether to parallel at codegen_output
//        Distributed =4 // TODO: Implement it
    };

    enum class RunnerType {
        Benchmark,
        Profiling // TODO: Implement it
    };

    struct CodeGenConfig {
        AdjMatType adjMatType = AdjMatType::VertexInduced;
        SchedulerType schedulerType = SchedulerType::GraphMini;
        PruningType pruningType = PruningType::Eager;
        ParallelType parType = ParallelType::NestedRt;
        RunnerType runnerType = RunnerType::Benchmark;
        bool bitmap = false; // Experimental non-IEP bitmap regions; IEP always uses arrays.
        bool bitmapDiagnostics = false; // Instrument only explicit verification runs.
        bool bitmapDirect = false; // Experimental shared projected-neighborhood live-ins.
        bool bitmapDeferredCounts = false; // Experimental capacity bounds instead of intermediate popcounts.
    };
}
