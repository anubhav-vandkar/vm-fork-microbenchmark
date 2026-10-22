#include "benchmark.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <set>



namespace vmbench {
void write_text(const fs::path& path, const std::string& contents) {
    std::ofstream output(path);
    require(bool(output), "Cannot write " + path.string());
    output << contents;
    require(bool(output), "Write failed " + path.string());
}
}

namespace vmbench {
namespace {
std::string current_memory(const Object& cgroup) {
    const auto& value = cgroup.at("memory.current");
    if (!value.is_string()) return {};
    auto result = text(value);
    result.erase(std::remove(result.begin(), result.end(), '\n'), result.end());
    return result;
}
}
CsvWriter::CsvWriter(const fs::path& path) : output_(path) {
    require(bool(output_), "Cannot open CSV output");
    output_ << "phase,round,position,fork_number,live_vms,pss_kib,rss_kib,private_dirty_kib,cgroup_current_bytes,kvm_sec_pagetables_bytes\n";
}
void CsvWriter::write(const Object& sample) {
    const auto& totals = sample.at("memory_sum_kib").as_object();
    output_ << text(sample.at("phase")) << ',' << integer(sample.at("round")) << ',' << integer(sample.at("position")) << ','
        << integer(sample.at("fork_number")) << ',' << integer(sample.at("live_vms")) << ',' << integer(totals.at("Pss")) << ','
        << integer(totals.at("Rss")) << ',' << integer(totals.at("Private_Dirty")) << ',';
    if (sample.at("cgroup").is_object()) {
        const auto& cgroup = sample.at("cgroup").as_object();
        output_ << current_memory(cgroup) << ',';
        if (const auto bytes = cgroup.at("memory.stat").as_object().if_contains("sec_pagetables")) output_ << integer(*bytes);
    } else output_ << ',';
    output_ << '\n';
    output_.flush();
    require(bool(output_), "CSV write failed");
}
}

namespace vmbench {
namespace {
json::value nullable(std::optional<int64_t> number) {
    return number ? json::value(*number) : json::value(nullptr);
}
}
void Statistics::observe(const Object& sample) {
    const auto phase = text(sample.at("phase"));
    const auto pss = integer(sample.at("memory_sum_kib").as_object().at("Pss"));
    if (phase == "root_ready") root_pss_ = pss;
    if (phase == "batch_ready" && (!peak_pss_ || pss > *peak_pss_)) peak_pss_ = pss;
    if (phase == "batch_destroyed") final_pss_ = pss;
    if (phase == "child_ready") readiness_times_.push_back(static_cast<uint64_t>(integer(sample.at("extra").as_object().at("guest_ready_ns"))));
}
Object Statistics::report() const {
    Object result{{"root_pss_kib", nullable(root_pss_)}, {"maximum_batch_pss_kib", nullable(peak_pss_)}, {"final_parent_pss_kib", nullable(final_pss_)}};
    if (readiness_times_.empty()) return result;
    auto sorted = readiness_times_;
    std::sort(sorted.begin(), sorted.end());
    result["guest_ready_median_ns"] = sorted[sorted.size() / 2];
    result["guest_ready_p95_ns"] = sorted[(sorted.size() * 95 + 99) / 100 - 1];
    return result;
}
}

namespace vmbench {
Recorder::Recorder(const Options& options)
    : options_(options), collector_(options.cgroup), raw_(options.output / "samples.jsonl"), csv_(options.output / "samples.csv") {
    require(bool(raw_), "Cannot open raw measurement output");
}
Object Recorder::collect_event(const MeasurementEvent& event) const {
    auto sample = collector_.collect(event.vms, event.phase != "child_ready");
    sample["phase"] = event.phase;
    sample["round"] = event.round;
    sample["position"] = event.position;
    sample["fork_number"] = event.fork_number;
    sample["extra"] = event.extra;
    return sample;
}
void Recorder::write_json(const Object& sample) {
    raw_ << json::serialize(sample) << '\n';
    raw_.flush();
    require(bool(raw_), "Raw measurement write failed");
}
void Recorder::print_progress(const MeasurementEvent& event, const Object& sample) const {
    if (event.phase == "child_ready" && event.position != options_.per_round) return;
    std::cout << event.phase << " round=" << event.round << " forks=" << event.fork_number << " live=" << event.vms.size()
        << " PSS_KiB=" << integer(sample.at("memory_sum_kib").as_object().at("Pss")) << std::endl;
}
void Recorder::record(const MeasurementEvent& event) {
    const auto sample = collect_event(event);
    write_json(sample);
    csv_.write(sample);
    statistics_.observe(sample);
    print_progress(event, sample);
}
}

namespace vmbench {
namespace {
using Series = std::map<std::string, std::vector<double>>;

Object read_object(const fs::path& path) { return json::parse(read_text(path)).as_object(); }

void validate_saved_sample(const Object& sample, int64_t index, int64_t rounds, int64_t children) {
    const auto last = 2 + rounds * (children + 3);
    require(index <= last, "Unexpected samples after final teardown");
    std::string phase;
    int64_t live = 0, round = 0, position = 0, forks = 0;
    if (index == 0) phase = "before_boot";
    else if (index == 1) { phase = "root_ready"; live = 1; }
    else if (index == last) { phase = "all_stopped"; round = rounds; forks = rounds * children; }
    else {
        round = (index - 2) / (children + 3) + 1;
        const auto offset = (index - 2) % (children + 3);
        forks = (round - 1) * children + std::min(offset, children);
        live = 1;
        if (offset == 0) phase = "round_before";
        else if (offset <= children) { phase = "child_ready"; live += offset; position = offset; }
        else if (offset == children + 1) { phase = "batch_ready"; live += children; position = children; }
        else phase = "batch_destroyed";
    }
    require(text(sample.at("phase")) == phase && integer(sample.at("live_vms")) == live &&
        integer(sample.at("round")) == round && integer(sample.at("position")) == position &&
        integer(sample.at("fork_number")) == forks && sample.at("processes").as_array().size() == static_cast<size_t>(live),
        "Saved lifecycle sequence or live VM count is invalid at sample " + std::to_string(index));
}

void collect_snapshot_series(const Object& metadata, Series& series) {
    if (const auto value = metadata.if_contains("prepare_ns")) series["snapshot_preparation"].push_back(integer(*value) / 1e6);
    if (const auto value = metadata.if_contains("snapshot_memory_bytes")) series["snapshot_memory_file"].push_back(integer(*value) / 1048576.0);
    if (const auto value = metadata.if_contains("snapshot_state_bytes")) series["snapshot_state_file"].push_back(integer(*value));
    if (const auto value = metadata.if_contains("snapshot_allocated_bytes")) series["snapshot_allocated_file"].push_back(integer(*value) / 1048576.0);
    if (const auto value = metadata.if_contains("capture_ns")) series["snapshot_capture_ms"].push_back(integer(*value) / 1e6);
    if (const auto value = metadata.if_contains("backing_copy_ns")) series["snapshot_backing_copy_ms"].push_back(integer(*value) / 1e6);
    if (const auto value = metadata.if_contains("base_allocated_bytes")) series["snapshot_base_allocated_mib"].push_back(integer(*value) / 1048576.0);
}

struct DiffAudit {
    int64_t initial = 0, reflinks = 0, sparse_copies = 0;
    std::string base;
    void observe(const Object& metadata, int64_t preparation, int64_t fork) {
        require(metadata.at("track_dirty_pages").as_bool(), "Diff snapshot lacks KVM dirty tracking");
        int64_t components = 0;
        for (const auto* field : {"capture_ns", "backing_copy_ns"}) {
            const auto value = integer(metadata.at(field));
            require(value >= 0 && value <= preparation - components, "Invalid diff preparation components");
            components += value;
        }
        require(metadata.at("initial_base").as_bool() == (fork == 1), "Diff base must be initialized exactly at the first fork");
        initial += metadata.at("initial_base").as_bool();
        const auto path = text(metadata.at("base_memory_path"));
        require(!path.empty() && path != text(metadata.at("snapshot_memory_path")) && (base.empty() || base == path),
            "Diff cumulative base was replaced or used as live child backing");
        base = path;
        const auto length = integer(metadata.at("snapshot_memory_bytes"));
        require(metadata.at("merge_in_capture").as_bool() && !metadata.at("child_track_dirty_pages").as_bool(), "Invalid native diff merge/child tracking metadata");
        require(length > 0 && integer(metadata.at("base_memory_bytes")) == length, "Diff base and child backing sizes disagree");
        const auto allocated = integer(metadata.at("base_allocated_bytes"));
        require(allocated >= 0 && allocated <= length, "Invalid diff allocation metadata");
        const auto method = text(metadata.at("snapshot_copy_method"));
        require(method == "reflink" || method == "sparse-copy", "Invalid diff backing copy method");
        reflinks += method == "reflink";
        sparse_copies += method == "sparse-copy";
    }
};

void collect_batch_series(const Object& sample, double baseline, int64_t children, Series& series) {
    const auto pss = integer(sample.at("memory_sum_kib").as_object().at("Pss")) / 1024.0;
    series["batch_pss"].push_back(pss);
    series["added_pss_per_child"].push_back((pss - baseline) / children);
    if (sample.at("cgroup").is_object()) {
        const auto& group = sample.at("cgroup").as_object();
        if (group.at("memory.current").is_string()) series["cgroup_batch"].push_back(std::stod(text(group.at("memory.current"))) / 1048576.0);
        if (const auto value = group.at("memory.stat").as_object().if_contains("sec_pagetables"); value && value->is_number())
            series["kvm_sec_pagetables_batch"].push_back(integer(*value) / 1048576.0);
    }
    for (const auto* field : {"kvm_vm_fd_count", "kvm_vcpu_fd_alias_count", "threads", "fd_count"}) {
        double total = 0;
        for (const auto& process : sample.at("processes").as_array()) total += integer(process.as_object().at(field));
        series[field].push_back(total);
    }
    if (const auto preparation = sample.at("extra").as_object().if_contains("preparation")) {
        collect_snapshot_series(preparation->as_object(), series);
    }
}

void summarize_series(Series& series, Object& result) {
    for (auto& [name, values] : series) {
        std::sort(values.begin(), values.end());
        const auto size = values.size();
        const auto median = size % 2 ? values[size / 2] : (values[size / 2 - 1] + values[size / 2]) / 2;
        result[name + "_median"] = median;
        result[name + "_max"] = values.back();
    }
}

json::value final_page_tables(const Object& sample) {
    if (sample.at("cgroup").is_object()) {
        if (const auto value = sample.at("cgroup").as_object().at("memory.stat").as_object().if_contains("sec_pagetables")) return *value;
    }
    return nullptr;
}

std::string display_number(const Object& result, const std::string& key) {
    const auto value = result.if_contains(key);
    if (!value || value->is_null()) return "Unavailable";
    std::ostringstream output;
    if (value->is_double()) output << std::fixed << std::setprecision(value->as_double() == std::trunc(value->as_double()) ? 0 : 2) << value->as_double();
    else output << integer(*value);
    return output.str();
}

struct ReportRow { const char* label; const char* key; const char* second = nullptr; };
const std::vector<ReportRow> report_rows = {
    {"Combined creation incl. preparation, mean (ms/child)", "amortized_creation_mean_ms"},
    {"Completed child creations", "forks_completed"}, {"Maximum live VMs", "maximum_live_vms"},
    {"Batch PSS median / max (MiB)", "batch_pss_median", "batch_pss_max"},
    {"Final parent PSS (MiB)", "final_parent_pss_mib"},
    {"Added PSS / child, median (MiB)", "added_pss_per_child_median"},
    {"Batch cgroup memory median / max (MiB)", "cgroup_batch_median", "cgroup_batch_max"},
    {"Secondary page tables, max (MiB)", "kvm_sec_pagetables_batch_max"},
    {"KVM VM descriptors per batch, max", "kvm_vm_fd_count_max"},
    {"vCPU descriptor aliases per batch, max", "kvm_vcpu_fd_alias_count_max"},
    {"Process threads per batch, max", "threads_max"}, {"Process descriptors per batch, max", "fd_count_max"},
    {"Guest ready median / p95 (ms)", "guest_ready_median_ms", "guest_ready_p95_ms"},
    {"Snapshot preparation median (ms)", "snapshot_preparation_median"},
    {"Snapshot preparations", "snapshot_preparations"},
    {"Snapshot mode", "snapshot_type"},
    {"Snapshot RAM file, median (MiB)", "snapshot_memory_file_median"},
    {"Child backing allocated, median (MiB)", "snapshot_allocated_file_median"},
    {"Cumulative base allocation, max (MiB)", "snapshot_base_allocated_mib_max"},
    {"Snapshot capture incl. native merge, median (ms)", "snapshot_capture_ms_median"},
    {"Backing copy median (ms)", "snapshot_backing_copy_ms_median"},
    {"Initial base snapshots", "initial_base_snapshots"},
    {"Reflink / sparse child copies", "reflink_copies", "sparse_copies"},
    {"Snapshot state file, median (bytes)", "snapshot_state_file_median"},
    {"Measured duration (seconds)", "elapsed_seconds"},
    {"Cgroup CPU time (seconds)", "cgroup_cpu_seconds"},
    {"Final live VMs", "final_live_vms"}, {"Final secondary page tables (bytes)", "final_sec_pagetables"}
};

std::string row_value(const Object& result, const ReportRow& row) {
    if (std::string(row.key) == "snapshot_type") return text(result.at("snapshot_type"));
    if (std::string(row.key) == "snapshot_preparation_median" && text(result.at("backend")) == "lightning") return "Not used (live fork)";
    const auto value = display_number(result, row.key);
    return row.second ? value + " / " + display_number(result, row.second) : value;
}

void replace_token(std::string& output, const std::string& token, const std::string& replacement) {
    size_t position = 0;
    while ((position = output.find(token, position)) != std::string::npos) {
        output.replace(position, token.size(), replacement);
        position += replacement.size();
    }
}
}

Object analyze_saved_backend(const fs::path& directory) {
    const auto summary = read_object(directory / "summary.json");
    require(text(summary.at("status")) == "ok" && summary.at("cleanup_errors").as_array().empty(), "Backend did not finish successfully");
    const auto& settings = summary.at("preflight").as_object();
    const bool firecracker = text(summary.at("backend")) == "firecracker";
    const auto policy = summary.if_contains("snapshot_policy");
    const std::string snapshot_policy = policy ? text(*policy) : firecracker ? "per-round" : "live-fork";
    require(firecracker ? snapshot_policy == "per-fork" || snapshot_policy == "per-round" : snapshot_policy == "live-fork",
        "Unsupported saved snapshot policy");
    const bool fresh = firecracker && snapshot_policy == "per-fork";
    const auto type = summary.if_contains("snapshot_type");
    const std::string snapshot_type = type ? text(*type) : firecracker ? "full" : "none";
    require(firecracker ? snapshot_type == "full" || snapshot_type == "diff" : snapshot_type == "none", "Invalid saved snapshot type");
    const bool diff = snapshot_type == "diff";
    if (diff) require(fresh && summary.at("track_dirty_pages").as_bool(), "Diff mode requires per-fork KVM dirty tracking");
    if (fresh) require(summary.at("creation_timing_includes_preparation").as_bool(), "Fresh snapshots must be included in creation timing");
    const auto rounds = integer(settings.at("rounds")), children = integer(settings.at("forks_per_round"));
    require(rounds > 0 && rounds <= 1000 && children > 0 && children <= 1000, "Invalid saved workload");
    const auto& saved = summary.at("result").as_object();
    require(integer(saved.at("forks_completed")) == rounds * children && integer(saved.at("rounds_completed")) == rounds &&
        integer(saved.at("maximum_live_vms")) == children + 1, "Saved completion counts disagree with workload");
    std::ifstream input(directory / "samples.jsonl");
    require(bool(input), "Cannot read saved raw samples");
    Statistics statistics;
    Series series;
    DiffAudit diff_audit;
    Object first, last;
    int64_t count = 0;
    double baseline = 0;
    int64_t batch_creation_ns = 0, total_creation_ns = 0;
    bool debugfs_available = true;
    std::set<std::string> snapshot_states, snapshot_memories;
    std::string line;
    while (std::getline(input, line)) {
        auto sample = json::parse(line).as_object();
        validate_saved_sample(sample, count, rounds, children);
        if (count++ == 0) first = sample;
        statistics.observe(sample);
        const auto phase = text(sample.at("phase"));
        if (phase == "round_before") {
            baseline = integer(sample.at("memory_sum_kib").as_object().at("Pss")) / 1024.0;
            batch_creation_ns = 0;
        }
        if (phase == "child_ready") {
            const auto elapsed = integer(sample.at("extra").as_object().at("guest_ready_ns"));
            require(elapsed >= 0, "Invalid saved child creation duration");
            if (fresh) {
                const auto& metadata = sample.at("extra").as_object().at("backend_metadata").as_object();
                const auto preparation = integer(metadata.at("prepare_ns"));
                require(text(metadata.at("snapshot_policy")) == "per-fork" &&
                    metadata.at("preparation_included_in_guest_ready").as_bool() && preparation >= 0 && preparation <= elapsed,
                    "Invalid per-fork snapshot timing metadata");
                const auto state = text(metadata.at("snapshot_path")), memory = text(metadata.at("snapshot_memory_path"));
                require(!state.empty() && !memory.empty() && state != memory &&
                    snapshot_states.insert(state).second && snapshot_memories.insert(memory).second,
                    "Per-fork snapshot files were reused");
                const auto child_type = metadata.if_contains("snapshot_type");
                require((child_type ? text(*child_type) : "full") == snapshot_type, "Mixed snapshot modes in saved run");
                if (diff) diff_audit.observe(metadata, preparation, integer(sample.at("fork_number")));
                collect_snapshot_series(metadata, series);
            }
            batch_creation_ns += elapsed;
        }
        if (phase == "batch_ready") {
            if (fresh) {
                const auto& preparation = sample.at("extra").as_object().at("preparation").as_object();
                require(text(preparation.at("snapshot_policy")) == "per-fork" && !preparation.contains("prepare_ns"),
                    "Per-fork snapshot preparation was also charged to the round");
            } else if (firecracker) {
                const auto elapsed = integer(sample.at("extra").as_object().at("preparation").as_object().at("prepare_ns"));
                require(elapsed >= 0, "Invalid saved snapshot preparation duration");
                batch_creation_ns += elapsed;
            }
            total_creation_ns += batch_creation_ns;
            series["batch_creation_ms"].push_back(batch_creation_ns / 1e6);
            collect_batch_series(sample, baseline, children, series);
            const auto counters = sample.if_contains("kvm_debugfs");
            debugfs_available &= counters && counters->is_object() && !counters->as_object().contains("unavailable");
        }
        last = std::move(sample);
    }
    require(!input.bad() && count == 3 + rounds * (children + 3), "Incomplete saved lifecycle samples");
    const auto preparations = series.contains("snapshot_preparation") ? static_cast<int64_t>(series.at("snapshot_preparation").size()) : 0;
    require(preparations == (firecracker ? fresh ? rounds * children : rounds : 0), "Snapshot preparation count disagrees with policy");
    if (diff) require(diff_audit.initial == 1 && diff_audit.reflinks + diff_audit.sparse_copies == rounds * children,
        "Incomplete diff base/copy audit");
    const auto metrics = statistics.report();
    require(metrics == summary.at("metrics").as_object(), "Saved summary statistics disagree with raw samples");
    Object result{{"backend", summary.at("backend")}, {"status", "ok"}, {"lifecycle_audit", "passed"}, {"samples", count}, {"rounds_completed", rounds},
        {"forks_completed", rounds * children}, {"maximum_live_vms", children + 1}, {"final_live_vms", 0},
        {"workload", settings}, {"kvm_debugfs_available", debugfs_available}, {"final_sec_pagetables", final_page_tables(last)},
        {"root_pss_mib", integer(metrics.at("root_pss_kib")) / 1024.0},
        {"final_parent_pss_mib", integer(metrics.at("final_parent_pss_kib")) / 1024.0},
        {"guest_ready_median_ms", integer(metrics.at("guest_ready_median_ns")) / 1e6},
        {"guest_ready_p95_ms", integer(metrics.at("guest_ready_p95_ns")) / 1e6},
        {"snapshot_policy", snapshot_policy}, {"snapshot_preparations", preparations},
        {"snapshot_type", snapshot_type}, {"track_dirty_pages", diff},
        {"initial_base_snapshots", diff_audit.initial}, {"reflink_copies", diff_audit.reflinks}, {"sparse_copies", diff_audit.sparse_copies},
        {"combined_creation_total_ms", total_creation_ns / 1e6},
        {"amortized_creation_mean_ms", total_creation_ns / 1e6 / (rounds * children)},
        {"elapsed_seconds", (integer(last.at("monotonic_ns")) - integer(first.at("monotonic_ns"))) / 1e9}};
    summarize_series(series, result);
    auto cpu_usage = [](const Object& sample) -> std::optional<int64_t> {
        if (!sample.at("cgroup").is_object()) return {};
        const auto value = sample.at("cgroup").as_object().if_contains("cpu.stat");
        if (!value || !value->is_string()) return {};
        const auto fields = parse_numeric_lines(text(*value), false);
        if (const auto usage = fields.if_contains("usage_usec")) return integer(*usage);
        return {};
    };
    const auto cpu_before = cpu_usage(first), cpu_after = cpu_usage(last);
    if (cpu_before && cpu_after) result["cgroup_cpu_seconds"] = (*cpu_after - *cpu_before) / 1e6;
    return result;
}

void generate_saved_report(const fs::path& directory, const fs::path& dashboard_template) {
    require(fs::is_directory(directory), "Report directory does not exist");
    const auto status_path = directory / "reporting-status.json";
    write_text(status_path, "{\"status\":\"running\",\"mode\":\"report_only\",\"vm_runs_started\":0}\n");
    try {
        const auto lightning = analyze_saved_backend(directory / "lightning");
        const auto firecracker = analyze_saved_backend(directory / "firecracker");
        require(text(lightning.at("backend")) == "lightning" && text(firecracker.at("backend")) == "firecracker", "Saved backend identities do not match");
        const auto& workload = lightning.at("workload").as_object();
        for (const auto* key : {"rounds", "forks_per_round", "memory_mib", "vcpus", "huge_pages", "host_kernel", "architecture"})
            require(workload.at(key) == firecracker.at("workload").as_object().at(key), "Backend run settings differ: " + std::string(key));
        Object comparison{{"run_directory", fs::absolute(directory).string()}, {"vm_suite_validation", "passed"},
            {"reporting_status", "ok"}, {"vm_runs_started", 0}, {"backends", Object{{"lightning", lightning}, {"firecracker", firecracker}}}};
        if (fs::exists(directory / "runner-status.json")) comparison["original_launcher_status"] = read_object(directory / "runner-status.json");
        std::ostringstream html_rows, markdown;
        markdown << "# VM fork results\n\nReporting regenerated from saved raw measurements; no VMs were started.\n\n"
                 << "| Measurement | Lightning | Firecracker |\n|---|---:|---:|\n";
        for (const auto& row : report_rows) {
            const auto left = row_value(lightning, row), right = row_value(firecracker, row);
            html_rows << "<tr><th scope=\"row\">" << row.label << "</th><td class=\"text-end tabular-nums\">" << left
                      << "</td><td class=\"text-end tabular-nums\">" << right << "</td></tr>\n";
            markdown << "| " << row.label << " | " << left << " | " << right << " |\n";
        }
        const auto debugfs = lightning.at("kvm_debugfs_available").as_bool() && firecracker.at("kvm_debugfs_available").as_bool()
            ? "Available" : "Unavailable (not collected or permission denied)";
        const bool fresh = text(firecracker.at("snapshot_policy")) == "per-fork";
        const bool diff = text(firecracker.at("snapshot_type")) == "diff";
        const std::string policy_label = fresh ? diff ? "fresh KVM dirty-page diff before every child" : "fresh full snapshot before every child"
            : "one shared snapshot per round (historical policy)";
        auto dashboard = read_text(dashboard_template);
        replace_token(dashboard, "{{rows}}", html_rows.str());
        replace_token(dashboard, "{{rounds}}", display_number(workload, "rounds"));
        replace_token(dashboard, "{{children}}", display_number(workload, "forks_per_round"));
        replace_token(dashboard, "{{memory}}", display_number(workload, "memory_mib"));
        replace_token(dashboard, "{{vcpus}}", display_number(workload, "vcpus"));
        replace_token(dashboard, "{{debugfs}}", debugfs);
        replace_token(dashboard, "{{snapshot_policy}}", policy_label);
        replace_token(dashboard, "{{creation_boundary}}", fresh ? "Child readiness includes snapshot preparation" : "Creation includes round preparation plus child readiness");
        replace_token(dashboard, "{{lightning_creation}}", display_number(lightning, "amortized_creation_mean_ms"));
        replace_token(dashboard, "{{firecracker_creation}}", display_number(firecracker, "amortized_creation_mean_ms"));
        markdown << "\nPSS includes mapped guest RAM and VMM userspace; cgroup memory also includes the controller, kernel and charged cache. "
                 << "These views overlap. Secondary page tables cover a subset of KVM-related memory, not every KVM allocation. "
                 << "vCPU descriptor aliases are references, not additional vCPUs. KVM debugfs: " << debugfs << ".\n\n"
                 << "Firecracker snapshot policy: " << policy_label << ". "
                 << (fresh ? "Every child readiness duration includes its own snapshot preparation. Combined creation mean = sum of child readiness durations / completed children; preparation is not added twice. "
                           : "Historical combined creation mean = (sum of round preparation durations + sum of child readiness durations) / completed children. Preparation is charged once per round. ")
                 << "Lightning has no snapshot preparation. This is a mean computed from raw durations, not a sum of medians. "
                 << "It includes backend clone/restore work and guest readiness; it excludes initial parent boot, cleanup, settling delays and metadata collection. "
                 << "Snapshot preparation is also reported as a component duration. The existing cleanup duration includes the settling delay. "
                 << "This is one instrumented run, not a general performance ranking.\n\n"
                 << "Both raw streams passed complete lifecycle ordering, live-count and completion checks; final live VMs are zero. "
                 << "Original summaries, raw samples, process logs and launcher status are preserved. The historical launcher failure, "
                 << "if present, is distinct from this successful reporting pass.\n";
        if (diff) markdown << "\nThe first diff of the booted parent forms a sparse cumulative base. Firecracker merges every later diff directly into that private base, "
            "then reflinked or copied sparsely into immutable child backing. The base is never mapped by a child. "
            "Capture includes native merging; capture, backing-copy and synchronization costs are included in preparation and guest readiness exactly once. "
            "The initial base is charged to the first child. Allocated bytes use stat blocks and are not unique physical bytes when reflinks share extents.\n";
        write_text(directory / "comparison.json", json::serialize(comparison) + "\n");
        write_text(directory / "report.md", markdown.str());
        write_text(directory / "dashboard.html", dashboard);
        write_text(status_path, "{\"status\":\"ok\",\"mode\":\"report_only\",\"vm_runs_started\":0}\n");
    } catch (const std::exception& error) {
        write_text(status_path, json::serialize(Object{{"status", "failed"}, {"mode", "report_only"}, {"vm_runs_started", 0}, {"error", error.what()}}) + "\n");
        throw;
    }
}
}
