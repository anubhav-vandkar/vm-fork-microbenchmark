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
    auto sample = collector_.collect(event.vms, event.phase != "child_ready" && event.phase != "depth_ready");
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

namespace vmbench {
namespace {
using DepthSeries = std::map<std::string, std::map<int, Series>>;

int expected_depth_sample(const Object& sample, size_t index, int maximum_depth) {
    std::string phase;
    int depth = 0, live = 0;
    if (index == 0) phase = "before_boot";
    else if (index == 1) { phase = "root_ready"; live = 1; }
    else if (index <= static_cast<size_t>(maximum_depth * 2 + 1)) {
        depth = static_cast<int>((index - 2) / 2) + 1;
        phase = index % 2 == 0 ? "depth_ready" : "depth_verified";
        live = depth + 1;
    } else if (index == static_cast<size_t>(maximum_depth * 2 + 2)) {
        phase = "lineage_verified"; depth = maximum_depth; live = depth + 1;
    } else {
        require(index == static_cast<size_t>(maximum_depth * 2 + 3), "Extra depth samples after teardown");
        phase = "all_stopped";
    }
    require(text(sample.at("phase")) == phase && integer(sample.at("round")) == 0 &&
        integer(sample.at("position")) == depth && integer(sample.at("live_vms")) == live &&
        integer(sample.at("fork_number")) == (phase == "all_stopped" ? maximum_depth : depth),
        "Depth lifecycle is incomplete or out of order");
    const auto& processes = sample.at("processes").as_array();
    require(processes.size() == static_cast<size_t>(live), "Depth process count differs from live VMs");
    std::set<int64_t> pids;
    for (size_t i = 0; i < processes.size(); ++i) {
        const auto& process = processes[i].as_object();
        const auto pid = integer(process.at("pid"));
        require(integer(process.at("vm_id")) == static_cast<int64_t>(i) && pid > 0 && pids.insert(pid).second,
            "Depth ancestry IDs or PIDs are inconsistent");
    }
    return depth;
}

const Object& newest_memory(const Object& sample) {
    return sample.at("processes").as_array().back().as_object().at("memory_kib").as_object();
}
void collect_depth_memory(const Object& sample, Series& series, const std::string& prefix) {
    const auto& memory = newest_memory(sample);
    const double private_kib = integer(memory.at("Private_Dirty")) + integer(memory.at("Private_Clean"));
    series[prefix + "private_mib"].push_back(private_kib / 1024.0);
    series[prefix + "pss_mib"].push_back(integer(memory.at("Pss")) / 1024.0);
    if (const auto huge = memory.if_contains("AnonHugePages")) {
        series[prefix + "anon_huge_mib"].push_back(integer(*huge) / 1024.0);
        series[prefix + "thp_observed"].push_back(integer(*huge) > 0 ? 1.0 : 0.0);
    }
    series[prefix + "chain_pss_mib"].push_back(integer(sample.at("memory_sum_kib").as_object().at("Pss")) / 1024.0);
}

void validate_depth_snapshot(const Object& metadata, int depth, const Object& settings,
    std::set<std::string>& backing_paths, std::string& base_path) {
    const auto type = text(settings.at("snapshot_type"));
    const bool diff = type == "diff";
    require(text(metadata.at("snapshot_type")) == type && metadata.at("track_dirty_pages").as_bool() == diff &&
        metadata.at("child_track_dirty_pages").as_bool() == diff &&
        metadata.at("sync_snapshot_files").as_bool() && metadata.at("preparation_included_in_guest_ready").as_bool(),
        "Depth snapshot tracking, synchronization or timing policy is inconsistent");
    const auto path = text(metadata.at("snapshot_memory_path"));
    require(backing_paths.insert(path).second && integer(metadata.at("snapshot_memory_bytes")) ==
        integer(settings.at("memory_mib")) * 1048576, "Depth backing reused or configured size changed");
    const auto prepare = integer(metadata.at("prepare_ns"));
    require(prepare >= 0 && integer(metadata.at("capture_ns")) >= 0 && integer(metadata.at("capture_ns")) <= prepare,
        "Invalid depth capture timing");
    if (!diff) return;
    const auto base = text(metadata.at("base_memory_path"));
    if (depth == 1) base_path = base;
    const auto method = text(metadata.at("snapshot_copy_method"));
    require(base == base_path && base != path && metadata.at("initial_base").as_bool() == (depth == 1) &&
        (method == "reflink" || method == "sparse-copy") &&
        integer(metadata.at("backing_copy_ns")) >= 0 &&
        integer(metadata.at("capture_ns")) + integer(metadata.at("backing_copy_ns")) <= prepare,
        "Invalid depth diff base, copy method or preparation accounting");
}

Object analyze_depth_chain(const fs::path& directory, const Object& settings, const std::string& backend,
    std::map<int, Series>& series) {
    const auto summary = read_object(directory / "summary.json");
    require(text(summary.at("status")) == "ok" && summary.at("cleanup_errors").as_array().empty() &&
        text(summary.at("experiment")) == "depth" && text(summary.at("backend")) == backend,
        "Depth chain failed or cleanup is incomplete: " + directory.string());
    const auto& preflight = summary.at("preflight").as_object();
    for (const auto* key : {"depth", "populate_mib", "memory_mib", "vcpus", "huge_pages", "settle_ms"})
        require(preflight.at(key) == settings.at(key), "Depth settings differ: " + std::string(key));
    const int maximum_depth = static_cast<int>(integer(settings.at("depth")));
    const auto& result = summary.at("result").as_object();
    require(integer(result.at("depth_completed")) == maximum_depth && integer(result.at("forks_completed")) == maximum_depth &&
        integer(result.at("maximum_live_vms")) == maximum_depth + 1 && integer(result.at("final_live_vms")) == maximum_depth + 1 &&
        result.at("ancestors_retained").as_bool(), "Depth summary does not match retained-chain workload");
    std::ifstream input(directory / "samples.jsonl");
    require(bool(input), "Cannot read depth samples");
    size_t index = 0;
    int64_t previous_time = -1;
    std::set<std::string> backing_paths;
    std::vector<int64_t> lineage_pids;
    std::string base_path, line;
    json::value root_huge = nullptr;
    json::value final_kvm = nullptr;
    while (std::getline(input, line)) {
        const auto sample = json::parse(line).as_object();
        const int depth = expected_depth_sample(sample, index++, maximum_depth);
        const auto time = integer(sample.at("monotonic_ns"));
        require(time >= previous_time, "Depth sample clock moved backwards"); previous_time = time;
        const auto phase = text(sample.at("phase"));
        const auto& processes = sample.at("processes").as_array();
        for (size_t i = 0; i < processes.size(); ++i) {
            const auto pid = integer(processes[i].as_object().at("pid"));
            if (i == lineage_pids.size()) lineage_pids.push_back(pid);
            require(lineage_pids[i] == pid, "Retained ancestor PID changed within the chain");
        }
        const auto& extra = sample.at("extra").as_object();
        if (phase == "root_ready") {
            require(extra.at("population_verified").as_bool(), "Root population was not verified");
            if (const auto huge = newest_memory(sample).if_contains("AnonHugePages")) root_huge = *huge;
        } else if (phase == "depth_ready") {
            require(integer(extra.at("depth")) == depth && integer(extra.at("source_id")) == depth - 1 &&
                integer(extra.at("child_id")) == depth && extra.at("inheritance_verified").as_bool(),
                "Newest child did not fork from its immediate predecessor");
            const auto ready = integer(extra.at("guest_ready_ns")), returned = integer(extra.at("fork_return_ns"));
            require(ready >= returned && returned >= 0, "Depth readiness timing is invalid");
            const auto& metadata = extra.at("backend_metadata").as_object();
            if (backend == "firecracker") {
                validate_depth_snapshot(metadata, depth, settings, backing_paths, base_path);
                require(integer(metadata.at("prepare_ns")) <= ready, "Snapshot preparation is outside readiness timing");
            }
            auto& values = series[depth];
            values["guest_ready_ms"].push_back(ready / 1e6);
            values["fork_return_ms"].push_back(returned / 1e6);
            if (const auto pause = metadata.if_contains("parent_pause_us")) values["parent_pause_ms"].push_back(integer(*pause) / 1000.0);
            collect_depth_memory(sample, values, "ready_");
        } else if (phase == "depth_verified") {
            require(integer(extra.at("depth")) == depth && extra.at("scan_verified").as_bool() && extra.at("isolation_verified").as_bool(),
                "Depth scan or write isolation failed");
            collect_depth_memory(sample, series[depth], "verified_");
        } else if (phase == "lineage_verified") {
            require(extra.at("ancestors_retained").as_bool(), "Retained lineage was not verified");
        } else if (phase == "all_stopped") {
            require(integer(sample.at("memory_sum_kib").as_object().at("Pss")) == 0, "Stopped VMs still appear in process accounting");
            final_kvm = final_page_tables(sample);
        }
    }
    require(index == static_cast<size_t>(maximum_depth * 2 + 4), "Depth stream is incomplete");
    return {{"directory", directory.string()}, {"status", "ok"}, {"final_live_vms", 0},
        {"root_anon_huge_kib", root_huge}, {"final_sec_pagetables", final_kvm}, {"samples", index}};
}

Object summarize_depth_values(const Series& series) {
    Object result;
    for (const auto& [name, original] : series) {
        auto values = original;
        require(!values.empty(), "Missing depth measurements");
        std::sort(values.begin(), values.end());
        const auto size = values.size();
        result[name + "_median"] = size % 2 ? values[size / 2] : (values[size / 2 - 1] + values[size / 2]) / 2;
        result[name + "_p99"] = values[(size * 99 + 99) / 100 - 1];
        result[name + "_min"] = values.front();
        result[name + "_max"] = values.back();
        result[name + "_samples"] = size;
    }
    return result;
}

void write_depth_tables(const fs::path& directory, const Object& settings, const json::array& rows, const json::array& chains) {
    std::ostringstream csv, html, markdown;
    csv << "backend,depth,samples,guest_ready_ms_median,guest_ready_ms_p99,ready_private_mib_median,"
           "ready_pss_mib_median,verified_private_mib_median,verified_pss_mib_median,ready_anon_huge_mib_median,parent_pause_ms_median\n";
    html << "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><title>Figure 4(b): fork depth</title>"
            "<style>body{font:16px system-ui;max-width:1200px;margin:40px auto;padding:0 20px;color:#17232d}"
            "table{border-collapse:collapse;width:100%}td,th{padding:8px;border-bottom:1px solid #ddd;text-align:right}"
            "td:first-child,th:first-child{text-align:left}code{background:#eee;padding:2px 5px}.note{padding:14px;background:#f0f5fa}</style>"
            "<h1>Figure 4(b): fork latency and memory vs depth</h1><p>Independent retained chains; "
         << integer(settings.at("memory_mib")) << " MiB RAM, " << integer(settings.at("populate_mib")) << " MiB populated, "
         << integer(settings.at("vcpus")) << " vCPU(s), " << integer(settings.at("repetitions")) << " repetitions.</p>"
            "<p class=\"note\">THP is requested with Transparent. AnonHugePages is observed per process, not proof of the guest mapping's exact huge-page coverage. "
            "Zero means no anonymous huge pages observed; file-backed Firecracker restores may differ. Check this before claiming a matched-THP comparison.</p>"
            "<p>Ready memory is sampled after the first guest reply. Verified memory follows a full page-marker scan, child write-isolation check and settling. "
            "Private memory = Private_Dirty + Private_Clean; PSS also includes shared memory. Both include VMM userspace, not KVM allocations. "
            "Cgroup secondary page tables are contextual accounting: shared groups may include unrelated VMs and teardown accounting can lag.</p>"
            "<p>Timers include clone/restore and first guest reply; scans, isolation checks, sampling, population and cleanup are outside the timer. "
            "Firecracker preparation is included once. At ten samples per depth, nearest-rank p99 is the maximum, not a robust tail estimate.</p>"
            "<p><a href=\"depth.csv\">Download plot-ready CSV</a> · <a href=\"comparison.json\">Full metadata</a></p>";
    markdown << "# Figure 4(b): fork depth\n\nIndependent retained chains, immediate-parent state and private-write checks. "
                "THP is requested; inspect observed AnonHugePages before claiming matched backing. "
                "Nearest-rank p99 from ten repetitions equals the maximum and is not a stable tail estimate.\n\n";
    const std::string header = "| Backend | Depth | Samples | Ready median ms | Ready p99 ms | Private ready MiB | PSS ready MiB | Private after checks MiB | AnonHugePages ready MiB |\n"
                               "|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    markdown << header;
    html << "<table><thead><tr><th>Backend</th><th>Depth</th><th>Samples</th><th>Ready median ms</th><th>Ready p99 ms</th>"
            "<th>Private ready MiB</th><th>PSS ready MiB</th><th>Private after checks MiB</th><th>AnonHugePages ready MiB</th></tr></thead><tbody>";
    for (const auto& entry : rows) {
        const auto& row = entry.as_object();
        const auto backend = text(row.at("backend"));
        csv << backend << ',' << integer(row.at("depth"));
        for (const auto* key : {"guest_ready_ms_samples", "guest_ready_ms_median", "guest_ready_ms_p99", "ready_private_mib_median",
             "ready_pss_mib_median", "verified_private_mib_median", "verified_pss_mib_median", "ready_anon_huge_mib_median", "parent_pause_ms_median"}) {
            csv << ',';
            if (const auto value = row.if_contains(key)) csv << json::serialize(*value);
        }
        csv << '\n';
        markdown << "| " << backend;
        html << "<tr><td>" << backend << "</td>";
        for (const auto* key : {"depth", "guest_ready_ms_samples", "guest_ready_ms_median", "guest_ready_ms_p99", "ready_private_mib_median",
             "ready_pss_mib_median", "verified_private_mib_median", "ready_anon_huge_mib_median"}) {
            const auto value = display_number(row, key);
            markdown << " | " << value;
            html << "<td>" << value << "</td>";
        }
        markdown << " |\n"; html << "</tr>";
    }
    html << "</tbody></table><p>Validated chains: " << chains.size() << ". Every chain ended with zero live VMs.</p></html>";
    write_text(directory / "depth.csv", csv.str());
    write_text(directory / "report.md", markdown.str());
    write_text(directory / "dashboard.html", html.str());
}
}

void generate_depth_report(const fs::path& directory) {
    require(fs::is_directory(directory), "Depth suite directory does not exist");
    const auto status_path = directory / "reporting-status.json";
    write_text(status_path, "{\"status\":\"running\",\"vm_runs_started\":0}\n");
    try {
        const auto settings = read_object(directory / "settings.json");
        const auto depth = integer(settings.at("depth")), repetitions = integer(settings.at("repetitions"));
        require(text(settings.at("experiment")) == "depth" && text(settings.at("huge_pages")) == "Transparent" &&
            depth >= 1 && depth <= 64 && repetitions >= 1 && repetitions <= 1000,
            "Invalid Figure 4(b) settings");
        require(text(settings.at("snapshot_type")) == "full" || text(settings.at("snapshot_type")) == "diff", "Invalid depth snapshot mode");
        const auto& backends = settings.at("backends").as_array();
        require(!backends.empty() && backends.size() <= 2, "Choose one or both supported depth backends");
        std::set<std::string> identities;
        DepthSeries series;
        json::array chains, rows;
        for (const auto& value : backends) {
            const auto backend = text(value);
            require((backend == "lightning" || backend == "firecracker") && identities.insert(backend).second,
                "Unsupported or repeated depth backend");
            for (int repetition = 1; repetition <= repetitions; ++repetition) {
                std::ostringstream name; name << "rep-" << std::setw(3) << std::setfill('0') << repetition;
                auto chain = analyze_depth_chain(directory / backend / name.str(), settings, backend, series[backend]);
                chain["backend"] = backend; chain["repetition"] = repetition; chains.push_back(std::move(chain));
            }
            for (int generation = 1; generation <= depth; ++generation) {
                require(series[backend][generation]["guest_ready_ms"].size() == static_cast<size_t>(repetitions), "Missing depth repetition");
                auto row = summarize_depth_values(series[backend][generation]);
                row["backend"] = backend; row["depth"] = generation;
                rows.push_back(std::move(row));
            }
        }
        const Object report{{"experiment", "depth"}, {"validation", "passed"}, {"vm_runs_started", 0},
            {"settings", settings}, {"chains", chains}, {"rows", rows}};
        write_depth_tables(directory, settings, rows, chains);
        write_text(directory / "comparison.json", json::serialize(report) + "\n");
        write_text(status_path, "{\"status\":\"ok\",\"vm_runs_started\":0}\n");
    } catch (const std::exception& error) {
        write_text(status_path, json::serialize(Object{{"status", "failed"}, {"error", error.what()}, {"vm_runs_started", 0}}) + "\n");
        throw;
    }
}
}
