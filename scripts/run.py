#!/usr/bin/env python3
import argparse
import datetime
import json
import os
import platform
import re
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PRIMARY = {
    "tcp_rr": ["ops_per_s"],
    "tcp_bulk": ["mib_per_s"],
    "tls_bulk": ["mib_per_s"],
    "tls_handshake": ["handshakes_per_s"],
    "udp_flood": ["received_per_s"],
    "file_seq": ["write_mib_per_s", "read_mib_per_s"],
    "file_rand": ["iops"],
    "timer_accuracy": ["p99_late_us"],
    "timer_churn": ["fire_cpu_ns", "arm_cancel_ns"],
    "wake_pingpong": ["round_trips_per_s"],
    "wake_flood": ["posts_per_s"],
}

LOWER_IS_BETTER = {"p50_us", "p99_us", "p50_late_us", "p99_late_us", "max_late_us", "early", "fire_cpu_ns",
                   "arm_cancel_ns", "last_late_ms", "cpu_s", "loss_pct"}


def expected_work(workload, params):
    if workload in ("tcp_bulk", "tls_bulk"):
        return {"bytes": int(params["bytes"])}
    if workload == "file_seq":
        return {"written": int(params["bytes"]), "read": int(params["bytes"])}
    if workload == "timer_accuracy":
        return {"sleeps": int(params["count"])}
    if workload == "timer_churn":
        return {"fired": int(params["count"]), "cancelled": int(params["count"])}
    if workload == "wake_pingpong":
        return {"round_trips": int(params["count"])}
    if workload == "wake_flood":
        return {"posts": int(params["count"])}
    return {}


def check_run(workload, params, run):
    problems = []
    work = run.get("work", {})
    for key, value in expected_work(workload, params).items():
        if work.get(key) != value:
            problems.append(f"work.{key} is {work.get(key)}, expected {value}")
    if workload == "tcp_rr":
        requests, received = work.get("requests"), work.get("bytes")
        if isinstance(requests, int) and isinstance(received, int) and received != requests * int(params["size"]):
            problems.append(f"work.bytes {received} is not requests x size ({requests} x {params['size']})")
    if workload == "udp_flood":
        sent, received = work.get("sent"), work.get("received")
        if isinstance(sent, int) and isinstance(received, int) and received > sent:
            problems.append(f"received {received} more datagrams than were sent ({sent})")
    return problems


def quick_params(params, quick):
    out = dict(params)
    if "seconds" in out:
        out["seconds"] = quick["seconds"]
    for key in ("bytes", "file_bytes"):
        if key in out:
            out[key] = min(int(out[key]), int(quick["bytes"]))
    if "count" in out:
        out["count"] = max(1, int(out["count"]) // int(quick["count_divisor"]))
    return out


def format_value(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, float) and value != value:
        return "nan"
    if abs(value) >= 100:
        return f"{value:,.0f}"
    if abs(value) >= 10:
        return f"{value:.1f}"
    if isinstance(value, int):
        return str(value)
    return f"{value:.2f}"


def command_output(command):
    try:
        lines = subprocess.run(command, capture_output=True, text=True, timeout=30).stdout.strip().splitlines()
        return lines[0] if lines else "unknown"
    except Exception:
        return "unknown"


def read_text(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return "unknown"


def cmake_cache(build_dir):
    values = {}
    try:
        with open(os.path.join(build_dir, "CMakeCache.txt")) as f:
            for line in f:
                match = re.match(r"^(CMAKE_CXX_COMPILER|CMAKE_BUILD_TYPE|CMAKE_CXX_FLAGS|CMAKE_CXX_FLAGS_RELEASE):[A-Z]+=(.*)$", line)
                if match:
                    values[match.group(1)] = match.group(2).strip()
    except OSError:
        pass
    return values


def machine_info(cpp_build):
    cpu = "unknown"
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    cpu = line.split(":", 1)[1].strip()
                    break
    except OSError:
        cpu = platform.processor() or "unknown"
    memory = "unknown"
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal"):
                    memory = f"{int(line.split()[1]) // 1024 // 1024} GiB"
                    break
    except OSError:
        pass
    cache = cmake_cache(cpp_build)
    compiler = cache.get("CMAKE_CXX_COMPILER") or os.environ.get("CXX", "g++-14")
    return {
        "host": socket.gethostname(),
        "date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "os": platform.platform(),
        "kernel": platform.release(),
        "cpu": cpu,
        "cpus": os.cpu_count(),
        "memory": memory,
        "governor": read_text("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "rmem_max": read_text("/proc/sys/net/core/rmem_max"),
        "wmem_max": read_text("/proc/sys/net/core/wmem_max"),
        "io_uring_disabled": read_text("/proc/sys/kernel/io_uring_disabled"),
        "cxx": command_output([compiler, "--version"]),
        "cxx_build": f"{cache.get('CMAKE_BUILD_TYPE', 'unknown')} {cache.get('CMAKE_CXX_FLAGS', '')} {cache.get('CMAKE_CXX_FLAGS_RELEASE', '')}".strip(),
        "rustc": command_output(["rustc", "--version"]),
        "python": platform.python_version(),
    }


def run_once(binary, workload, params, env, cwd, timeout):
    command = [binary, workload] + [f"{k}={v}" for k, v in params.items()]
    load_before = os.getloadavg()[0]
    try:
        proc = subprocess.run(command, capture_output=True, text=True, env=env, cwd=cwd, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"status": "failed", "error": f"timed out after {timeout} s", "load": load_before}
    load = max(load_before, os.getloadavg()[0])
    if proc.returncode == 2:
        return {"status": "unsupported", "error": proc.stderr.strip()[-500:], "load": load}
    if proc.returncode != 0:
        return {"status": "failed", "error": f"exit {proc.returncode}: {proc.stderr.strip()[-1500:]}", "load": load}
    lines = [line for line in proc.stdout.strip().splitlines() if line.strip()]
    if not lines:
        return {"status": "failed", "error": "no output", "load": load}
    try:
        result = json.loads(lines[-1])
    except json.JSONDecodeError as error:
        return {"status": "failed", "error": f"bad JSON: {error}: {lines[-1][:300]}", "load": load}
    result["status"] = "ok"
    result["load"] = load
    return result


def aggregate(workload, params, runs):
    ok = [r for r in runs if r["status"] == "ok"]
    problems = []
    for index, run in enumerate(ok):
        problems.extend(f"run {index + 1}: {p}" for p in check_run(workload, params, run))
    if not ok:
        return {"status": runs[-1]["status"], "error": runs[-1].get("error", ""), "runs": len(runs)}, problems
    metrics, spread = {}, {}
    for key in ok[0]["metrics"]:
        values = [r["metrics"].get(key) for r in ok if isinstance(r["metrics"].get(key), (int, float))]
        if values:
            metrics[key] = statistics.median(values)
            spread[key] = [min(values), max(values)]
    summary = {
        "status": "ok" if len(ok) == len(runs) else "partial",
        "runs": len(ok),
        "library_version": ok[0].get("library_version", ""),
        "metrics": metrics,
        "spread": spread,
        "samples": [r["metrics"] for r in ok],
        "work": ok[0].get("work", {}),
        "max_load": max(r.get("load", 0.0) for r in runs),
    }
    if summary["status"] == "partial":
        failed = next(r for r in runs if r["status"] != "ok")
        summary["error"] = failed.get("error", "")
    return summary, problems


def target_name(target):
    return target["library"] if target["variant"] == "default" else f"{target['library']} ({target['variant']})"


def cell(entry, metric):
    summary = entry["summary"]
    if entry["problems"]:
        return "work mismatch"
    if summary["status"] in ("ok", "partial"):
        value = format_value(summary["metrics"].get(metric, float("nan")))
        return value + (" (partial)" if summary["status"] == "partial" else "")
    return summary["status"]


def write_markdown(path, info, results, targets, repeat):
    lines = ["# Results", ""]
    lines.append(f"{info['date']} on `{info['host']}`: {info['cpu']}, {info['cpus']} CPUs, {info['memory']}, "
                 f"Linux {info['kernel']}, governor {info['governor']}.")
    lines.append(f"C++: {info['cxx']}, {info['cxx_build']}. Rust: {info['rustc']}.")
    lines.append(f"UDP buffer limits: rmem_max {info['rmem_max']}, wmem_max {info['wmem_max']}. "
                 f"io_uring_disabled {info['io_uring_disabled']}.")
    versions = {}
    for entry in results:
        if entry["summary"].get("library_version"):
            versions[target_name(entry["target"])] = entry["summary"]["library_version"]
    if versions:
        lines.append("Versions: " + ", ".join(f"{k} {v}" for k, v in sorted(versions.items())) + ".")
    loads = [e["summary"].get("max_load") for e in results if e["summary"].get("max_load") is not None]
    if loads:
        lines.append(f"Load average during runs: {min(loads):.1f} to {max(loads):.1f}.")
    lines.append("")
    lines.append(f"Each number is the median of {repeat} run{'s' if repeat != 1 else ''}; the Detail tables give the "
                 "range. `METHOD.md` says what each workload does, and each library's `NOTES.md` the choices its "
                 "implementation made.")
    lines.append("")

    names = []
    for target in targets:
        name = target_name(target)
        if name not in names:
            names.append(name)

    lines.append("## Summary")
    lines.append("")
    lines.append("| workload | params | metric | " + " | ".join(names) + " |")
    lines.append("|" + "---|" * (3 + len(names)))
    by_key = {}
    for entry in results:
        key = (entry["workload"], json.dumps(entry["params"], sort_keys=True))
        by_key.setdefault(key, {})[target_name(entry["target"])] = entry
    for (workload, params_json), cells in by_key.items():
        params = json.loads(params_json)
        shown = " ".join(f"{k}={v}" for k, v in params.items())
        for metric in PRIMARY.get(workload, []):
            row = f"| {workload} | {shown} | {metric}{' (lower is better)' if metric in LOWER_IS_BETTER else ''} |"
            for name in names:
                entry = cells.get(name)
                row += " |" if entry is None else f" {cell(entry, metric)} |"
            lines.append(row)
    lines.append("")

    lines.append("## Detail")
    for (workload, params_json), cells in by_key.items():
        params = json.loads(params_json)
        lines.append("")
        lines.append(f"### {workload} " + " ".join(f"{k}={v}" for k, v in params.items()))
        lines.append("")
        metric_names = []
        for entry in cells.values():
            for key in entry["summary"].get("metrics", {}):
                if key not in metric_names:
                    metric_names.append(key)
        lines.append("| " + " | ".join(["library", "status", "load"] + metric_names) + " |")
        lines.append("|" + "---|" * (3 + len(metric_names)))
        for name in names:
            entry = cells.get(name)
            if entry is None:
                continue
            summary = entry["summary"]
            status = summary["status"] if not entry["problems"] else "work mismatch: " + "; ".join(entry["problems"])
            if summary.get("error"):
                status += ": " + summary["error"].replace("\n", " ").replace("|", "/")[:160]
            values = []
            for metric in metric_names:
                value = summary.get("metrics", {}).get(metric)
                if value is None:
                    values.append("")
                    continue
                low, high = summary.get("spread", {}).get(metric, [value, value])
                values.append(format_value(value) if low == high else f"{format_value(value)} [{format_value(low)}–{format_value(high)}]")
            load = summary.get("max_load")
            lines.append("| " + " | ".join([name, status, "" if load is None else f"{load:.1f}"] + values) + " |")
    lines.append("")
    with open(path, "w") as f:
        f.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description="Run the comparison benchmarks.")
    parser.add_argument("--quick", action="store_true", help="tiny parameters, one run each, for CI")
    parser.add_argument("--repeat", type=int, default=None, help="runs per measurement (default 5, or 1 with --quick)")
    parser.add_argument("--cpp-build", default=os.path.join(ROOT, "build"), help="CMake build directory")
    parser.add_argument("--rust-target", default=os.path.join(ROOT, "rust", "target"), help="cargo target directory")
    parser.add_argument("--only", default="", help="comma list of library or library:variant to run")
    parser.add_argument("--workload", default="", help="comma list of workloads to run")
    parser.add_argument("--out", default=os.path.join(ROOT, "results"), help="where to write results")
    parser.add_argument("--name", default="", help="results file name, without extension")
    parser.add_argument("--timeout", type=int, default=600, help="seconds before one run is abandoned")
    args = parser.parse_args()

    with open(os.path.join(ROOT, "workloads.json")) as f:
        config = json.load(f)
    repeat = args.repeat if args.repeat is not None else (1 if args.quick else 5)
    only = [x for x in args.only.split(",") if x]
    chosen_workloads = [x for x in args.workload.split(",") if x]

    targets = []
    for target in config["targets"]:
        if only and target["library"] not in only and f"{target['library']}:{target['variant']}" not in only:
            continue
        binary = target["binary"].replace("{cpp}", args.cpp_build).replace("{rust}", args.rust_target)
        targets.append(dict(target, binary=binary))

    info = machine_info(args.cpp_build)
    workdir = tempfile.mkdtemp(prefix="vio-bench-", dir=os.environ.get("BENCH_WORKDIR"))
    results = []
    failures = 0
    try:
        for workload, param_sets in config["workloads"].items():
            if chosen_workloads and workload not in chosen_workloads:
                continue
            for raw_params in param_sets:
                params = quick_params(raw_params, config["quick"]) if args.quick else dict(raw_params)
                for target in targets:
                    if "only" in target and workload not in target["only"]:
                        continue
                    shown = " ".join(f"{k}={v}" for k, v in params.items())
                    if not os.path.exists(target["binary"]):
                        summary = {"status": "missing", "error": f"{target['binary']} not built"}
                        failures += 1
                        print(f"{workload} {shown} {target_name(target)}: missing", file=sys.stderr)
                        results.append({"workload": workload, "params": params, "target": target, "summary": summary,
                                        "problems": []})
                        continue
                    env = dict(os.environ)
                    env.update(target.get("env", {}))
                    env["BENCH_VARIANT"] = target["variant"]
                    env.setdefault("BENCH_CERT_DIR", os.path.join(ROOT, "certs"))
                    runs = []
                    for _ in range(repeat):
                        run = run_once(target["binary"], workload, params, env, workdir, args.timeout)
                        runs.append(run)
                        if run["status"] != "ok":
                            break
                    summary, problems = aggregate(workload, params, runs)
                    if summary["status"] in ("failed", "partial") or problems:
                        failures += 1
                    primary = ", ".join(
                        f"{m}={format_value(summary['metrics'][m])}" for m in PRIMARY.get(workload, [])
                        if m in summary.get("metrics", {}))
                    detail = primary or summary.get("error", "")[:200]
                    print(f"{workload} {shown} {target_name(target)}: {summary['status']} {detail}"
                          + (f" [{'; '.join(problems)}]" if problems else ""), file=sys.stderr)
                    results.append({"workload": workload, "params": params, "target": target, "summary": summary,
                                    "problems": problems})
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    os.makedirs(args.out, exist_ok=True)
    name = args.name or (datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d") + "-" + info["host"]
                         + ("-quick" if args.quick else ""))
    with open(os.path.join(args.out, name + ".json"), "w") as f:
        json.dump({"machine": info, "repeat": repeat, "quick": args.quick, "results": results}, f, indent=1)
    write_markdown(os.path.join(args.out, name + ".md"), info, results, targets, repeat)
    print(f"wrote {os.path.join(args.out, name)}.md and .json; {failures} failed", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
