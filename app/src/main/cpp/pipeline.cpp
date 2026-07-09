#include "pipeline.h"

#include <algorithm>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

void Pipeline::addStage(std::unique_ptr<PipelineStage> stage) {
    LOGI("Pipeline: registered stage '%s'", std::string(stage->name()).c_str());
    stages_.push_back(std::move(stage));
}

void Pipeline::insertAfter(std::string_view after, std::unique_ptr<PipelineStage> stage) {
    size_t idx = indexOf(after);
    LOGI("Pipeline: inserting '%s' after '%s' (pos %zu)",
         std::string(stage->name()).c_str(), std::string(after).c_str(),
         idx == stages_.size() ? stages_.size() : idx + 1);
    auto it = (idx == stages_.size())
                  ? stages_.end()
                  : stages_.begin() + static_cast<ptrdiff_t>(idx + 1);
    stages_.insert(it, std::move(stage));
}

void Pipeline::insertBefore(std::string_view before, std::unique_ptr<PipelineStage> stage) {
    size_t idx = indexOf(before);
    LOGI("Pipeline: inserting '%s' before '%s' (pos %zu)",
         std::string(stage->name()).c_str(), std::string(before).c_str(), idx);
    auto it = (idx == stages_.size())
                  ? stages_.begin()
                  : stages_.begin() + static_cast<ptrdiff_t>(idx);
    stages_.insert(it, std::move(stage));
}

// ─────────────────────────────────────────────────────────────────────────────
// Runtime control
// ─────────────────────────────────────────────────────────────────────────────

void Pipeline::setEnabled(std::string_view name, bool enabled) {
    size_t idx = indexOf(name);
    if (idx == stages_.size()) {
        LOGE("Pipeline::setEnabled — unknown stage '%s'", std::string(name).c_str());
        return;
    }
    stages_[idx]->enabled = enabled;
    LOGI("Pipeline: stage '%s' %s", std::string(name).c_str(),
         enabled ? "enabled" : "disabled");
}

// ─────────────────────────────────────────────────────────────────────────────
// Execution
// ─────────────────────────────────────────────────────────────────────────────

bool Pipeline::run(FrameContext& ctx, std::function<void(const std::string&, int)> progressCallback) {
    using clock = std::chrono::steady_clock;
    auto pipelineStart = clock::now();

    int totalEnabledStages = 0;
    for (auto& stage : stages_) {
        if (stage->enabled) totalEnabledStages++;
    }

    int completedStages = 0;
    for (auto& stage : stages_) {
        if (!stage->enabled) {
            LOGI("Pipeline: skipping disabled stage '%s'", std::string(stage->name()).c_str());
            continue;
        }

        if (progressCallback) {
            int percentage = (completedStages * 100) / std::max(1, totalEnabledStages);
            progressCallback(std::string(stage->name()), percentage);
        }

        auto t0 = clock::now();
        bool ok = stage->process(ctx);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();

        LOGI("Pipeline: stage '%-12s' %s  (%lld ms)",
             std::string(stage->name()).c_str(),
             ok ? "OK" : "FAILED",
             static_cast<long long>(ms));

        if (!ok) {
            LOGE("Pipeline: aborting after failed stage '%s'",
                 std::string(stage->name()).c_str());
            return false;
        }
        completedStages++;
    }

    if (progressCallback) {
        progressCallback("complete", 100);
    }

    auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       clock::now() - pipelineStart)
                       .count();
    LOGI("Pipeline: complete — total %lld ms", static_cast<long long>(totalMs));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Inspection
// ─────────────────────────────────────────────────────────────────────────────

size_t Pipeline::indexOf(std::string_view name) const {
    for (size_t i = 0; i < stages_.size(); ++i) {
        if (stages_[i]->name() == name) return i;
    }
    return stages_.size(); // sentinel: not found
}
