#pragma once
#include "vmbench/benchmark.hpp"
#include "vmbench/csv_writer.hpp"
#include "vmbench/sample_collector.hpp"
#include "vmbench/statistics.hpp"

namespace vmbench {
class Recorder {
public:
    explicit Recorder(const Options& options);
    void record(const MeasurementEvent& event);
    Object report() const { return statistics_.report(); }
private:
    Object collect_event(const MeasurementEvent& event) const;
    void write_json(const Object& sample);
    void print_progress(const MeasurementEvent& event, const Object& sample) const;
    const Options& options_;
    SampleCollector collector_;
    std::ofstream raw_;
    CsvWriter csv_;
    Statistics statistics_;
};
}
