"""Bounded, resumable Wiki-Vote sweep; one isolated process per atlas/backend.

Export the corpus with --export-corpus FILE (requires networkx). On Linux run
with --corpus FILE --output DIR --real-dir DIR. Pin the parent with taskset;
workers inherit affinity. Execution budgets cover warmup plus all timed trials,
but exclude compilation, calibration and graph loading. Never run two drivers
in the same source tree: runtime compilation writes a shared generated plan.
"""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import signal
import statistics
import struct
import subprocess
import sys
import time


def save(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def read_progress(path):
    """Read only after native return or worker exit, never concurrently."""
    raw = Path(path).read_bytes()
    if len(raw) < 64:
        return dict(available=False, reason="incomplete progress header")
    version, threads, vertices, redundancy = struct.unpack_from("=4Q", raw)
    if version != 1 or threads < 1 or len(raw) != 64 + 64 * threads + vertices:
        return dict(available=False, reason="invalid progress snapshot")
    counts = [struct.unpack_from("=q", raw, 64 + 64*i)[0] for i in range(threads)]
    flags = raw[64 + 64*threads:]
    completed = [i for i, flag in enumerate(flags) if flag == 2]
    return dict(available=True, total_root_vertices=vertices,
                started_root_vertices=sum(flag != 0 for flag in flags),
                completed_root_vertices=len(completed), completed_root_vertex_ids=completed,
                accumulated_matches=sum(counts) // max(1, redundancy),
                semantics="Match-count lower bound from completed counting operations, including bitmap leaves before reduction. "
                          "Vertex IDs are internal outer-loop graph IDs, not distinct vertices in matches.")


def worker(args):
    import itertools
    import random
    import resource
    import graphmini as gm
    from benchmark_bitmap_server import hosts, make_graph
    from induced_subset_oracle import count_induced_subsets
    from matching_oracle import matrix

    job = json.loads(Path(args.worker).read_text())
    record = dict(job, status="preparing", pid=os.getpid())
    state = Path(args.worker).with_name("state.json")
    save(state, record)
    _, graph, graph_meta = next(hosts(args.real_dir, ["wiki-Vote"]))
    bits = job["pattern"]
    query = [[int(bits[i * 6 + j]) for j in range(6)] for i in range(6)]
    rng = random.Random(20260909)
    calibration = matrix(10, [e for e in itertools.combinations(range(10), 2)
                              if rng.random() < .6])
    for i in range(6):
        for j in range(6):
            calibration[i][j] = query[i][j]
    expected = count_induced_subsets(calibration, query)
    assert expected > 0
    small = make_graph([{j for j, bit in enumerate(row) if bit} for row in calibration])
    # Outgoing ordering is graph-independent, but nested_rt task thresholds use
    # graph statistics: compile for the measured host, then validate on the oracle.
    plan = gm.compile_plan(graph, bits, "vertex", scheduler="outgoing",
                           pruning_type="none", parallel_type=job["parallel"],
                           bitmap=job["backend"] == "bitmap",
                           bitmap_direct=job.get("bitmap_direct", False))
    assert plan.run(small, num_threads=1).number_of_matches == expected
    source = plan.generated_code
    state.with_name("plan.cpp").write_text(source)
    record.update(graph_metadata=graph_meta, oracle_calibration_count=expected,
                  compilation=plan.compilation_profile,
                  code_sha256=hashlib.sha256(source.encode()).hexdigest(),
                  bitmap_selected="// bitmap-region build once" in source,
                  direct_selected="// shared bounded neighborhood projection" in source,
                  full_region="// full bitmap region" in source,
                  fixed_words="count_local<bitmap_words>" in source,
                  module=gm.__file__, affinity=sorted(os.sched_getaffinity(0)),
                  load_average=os.getloadavg(), samples_seconds=[],
                  wall_samples_seconds=[], status="running",
                  execution_started=time.monotonic())
    save(state, record)
    def measured_run(label):
        progress = state.with_name(f"progress-{label}-{os.getpid()}-{time.time_ns()}.bin")
        record.update(active_run=label, active_run_started=time.monotonic(), progress_file=str(progress))
        save(state, record)
        old = os.environ.get("GRAPHMINI_PROGRESS_FILE")
        os.environ["GRAPHMINI_PROGRESS_FILE"] = str(progress)
        try:
            result = plan.run(graph, num_threads=job["threads"])
        finally:
            if old is None:
                os.environ.pop("GRAPHMINI_PROGRESS_FILE", None)
            else:
                os.environ["GRAPHMINI_PROGRESS_FILE"] = old
        snapshot = read_progress(progress) if progress.exists() else dict(available=False)
        if job['parallel'] != 'openmp':
            assert snapshot.get('available'), 'TBB progress snapshot missing'
        if snapshot.get("available"):
            assert snapshot["completed_root_vertices"] == snapshot["total_root_vertices"]
            assert snapshot["accumulated_matches"] == result.number_of_matches
        record.setdefault("completed_run_progress", {})[label] = snapshot
        return result

    warm = measured_run("warmup")
    record.update(count=warm.number_of_matches, warmup_seconds=warm.execution_time_seconds)
    save(state, record)
    for trial in range(job["trials"]):
        start = time.perf_counter()
        result = measured_run(f"trial-{trial}")
        wall = time.perf_counter() - start
        assert result.number_of_matches == record["count"], "Repeated count mismatch"
        record["samples_seconds"].append(result.execution_time_seconds)
        record["wall_samples_seconds"].append(wall)
        save(state, record)
    samples = record["samples_seconds"]
    median = statistics.median(samples)
    record.update(status="complete", median_seconds=median,
                  mad_seconds=statistics.median(abs(v - median) for v in samples),
                  execution_wall_seconds=time.monotonic() - record["execution_started"],
                  process_peak_rss_kib=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
    save(state, record)


def run_job(command, directory, execution_budget, preparation_budget):
    """External watchdog also stops native code that cannot handle Python signals."""
    state = directory / "state.json"
    started = time.monotonic()
    with (directory / "worker.log").open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        timeout = None
        try:
            while process.poll() is None:
                record = json.loads(state.read_text()) if state.exists() else {}
                execution_start = record.get("execution_started")
                if execution_start is not None:
                    expired = time.monotonic() - execution_start > execution_budget
                    kind = "execution_timeout"
                else:
                    expired = time.monotonic() - started > preparation_budget
                    kind = "preparation_timeout"
                if expired:
                    timeout = kind
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGKILL)
                    break
                time.sleep(.1)
        finally:
            if process.poll() is None:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(process.pid, signal.SIGKILL)
            process.wait()
    record = json.loads(state.read_text()) if state.exists() else {}
    if timeout:
        record["status"] = timeout
    elif process.returncode != 0 or record.get("status") != "complete":
        record["status"] = "error"
    record.update(returncode=process.returncode, job_wall_seconds=time.monotonic() - started)
    if record.get("execution_started") is not None:
        record["execution_wall_seconds"] = time.monotonic() - record["execution_started"]
    if record.get("progress_file") and Path(record["progress_file"]).exists():
        record["partial_progress"] = read_progress(record["progress_file"])
        record["partial_progress"]["run"] = record.get("active_run")
        record["partial_progress"]["is_final_count"] = record.get("status") == "complete"
        if record.get("active_run_started") is not None:
            record["partial_progress"]["run_wall_seconds"] = time.monotonic() - record["active_run_started"]
    save(state, record)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export-corpus")
    parser.add_argument("--corpus")
    parser.add_argument("--output")
    parser.add_argument("--real-dir")
    parser.add_argument("--worker")
    parser.add_argument("--threads", type=int, default=12)
    parser.add_argument("--parallel", choices=["openmp", "tbb_top", "nested", "nested_rt"], default="nested_rt")
    parser.add_argument("--backends", choices=["array,bitmap", "array", "bitmap"], default="array,bitmap")
    parser.add_argument("--bitmap-direct", action="store_true",
                        help="Opt in to supported shared-projection bitmap live-ins")
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--execution-budget", type=float, default=300)
    parser.add_argument("--preparation-budget", type=float, default=300)
    parser.add_argument("--atlas-ids", help="Optional comma-separated pilot subset")
    args = parser.parse_args()
    if args.export_corpus:
        import networkx as nx
        patterns = [dict(atlas_id=i, edges=g.number_of_edges(),
                         graph6=nx.to_graph6_bytes(g, header=False).decode().strip(),
                         pattern="".join(str(int(g.has_edge(a, b))) for a in range(6) for b in range(6)))
                    for i, g in enumerate(nx.graph_atlas_g()) if len(g) == 6 and nx.is_connected(g)]
        assert len(patterns) == 112
        save(args.export_corpus, dict(networkx_version=nx.__version__, patterns=patterns))
        return
    if args.worker:
        worker(args)
        return
    from runtime_test_support import restore_generated_plan_at_exit
    restore_generated_plan_at_exit()
    assert args.threads > 0 and args.trials >= 1
    assert args.execution_budget > 0 and args.preparation_budget > 0
    corpus = Path(args.corpus).read_bytes()
    patterns = json.loads(corpus)["patterns"]
    assert len(patterns) == 112 and len({p["pattern"] for p in patterns}) == 112
    if args.atlas_ids:
        selected = set(map(int, args.atlas_ids.split(",")))
        patterns = [p for p in patterns if p["atlas_id"] in selected]
        assert len(patterns) == len(selected)
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    metadata = dict(commit=subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                    corpus_sha256=hashlib.sha256(corpus).hexdigest(), threads=args.threads,
                    trials=args.trials, parallel=args.parallel, backends=args.backends,
                    bitmap_direct=args.bitmap_direct,
                    bitmap_task_policy=os.environ.get("GRAPHMINI_BITMAP_TASK_POLICY", "baseline"),
                    execution_budget=args.execution_budget,
                    preparation_budget=args.preparation_budget, atlas_ids=args.atlas_ids,
                    affinity=sorted(os.sched_getaffinity(0)),
                    driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    real_dir=str(Path(args.real_dir).resolve()),
                    hostname=os.uname().nodename,
                    slurm={k: os.environ.get(k) for k in ("SLURM_JOB_ID", "SLURM_ARRAY_TASK_ID", "SLURM_CPUS_PER_TASK")},
                    omp={k: os.environ.get(k) for k in ("OMP_PLACES", "OMP_PROC_BIND", "OMP_DYNAMIC")})
    meta_path = output / "metadata.json"
    if meta_path.exists():
        assert json.loads(meta_path.read_text()) == metadata, "Resume configuration changed"
    save(meta_path, metadata)
    with (output / "results.jsonl").open("w") as results:
        for pattern in patterns:
            pair = []
            # Alternate backend order by pattern; no concurrent native compilations.
            order = ["array", "bitmap"] if pattern["atlas_id"] % 2 else ["bitmap", "array"]
            order = [backend for backend in order if backend in args.backends.split(",")]
            for backend in order:
                directory = output / f'{pattern["atlas_id"]}-{backend}'
                directory.mkdir(exist_ok=True)
                job = dict(pattern, backend=backend, threads=args.threads, trials=args.trials, parallel=args.parallel,
                           bitmap_direct=args.bitmap_direct and backend == "bitmap")
                save(directory / "job.json", job)
                state = directory / "state.json"
                record = json.loads(state.read_text()) if state.exists() else {}
                if record.get("status") not in ("complete", "execution_timeout", "preparation_timeout", "error"):
                    # Discard only this interrupted job's stale watchdog timestamp.
                    save(state, dict(job, status="queued"))
                    record = run_job([sys.executable, str(Path(__file__).resolve()),
                                      "--worker", str(directory / "job.json"),
                                      "--real-dir", args.real_dir], directory,
                                     args.execution_budget, args.preparation_budget)
                record = dict(job, **{k: v for k, v in record.items() if k not in job})
                results.write(json.dumps(record) + "\n")
                results.flush()
                print(f'{pattern["atlas_id"]} {backend}: {record["status"]}', flush=True)
                progress = record.get("partial_progress", {})
                if record['status'] == 'execution_timeout' and args.parallel != 'openmp':
                    assert progress.get('available'), f'Missing timeout progress: {directory}'
                if progress.get("available"):
                    print(f'  {progress["completed_root_vertices"]}/{progress["total_root_vertices"]} '
                          f'outer vertices completed; {progress["accumulated_matches"]} committed matches '
                          f'({progress.get("run")})', flush=True)
                pair.append(record)
                if record["status"] in ("error", "preparation_timeout"):
                    raise RuntimeError(f"Investigate {directory / 'worker.log'}")
            if len(pair) == 2 and all("count" in r for r in pair):
                assert pair[0]["count"] == pair[1]["count"], f"Backend count mismatch: {pattern}"
    print("SWEEP_COMPLETE", flush=True)


if __name__ == "__main__":
    main()
