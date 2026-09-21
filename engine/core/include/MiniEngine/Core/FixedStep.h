#pragma once

#include <MiniEngine/Core/Assert.h>
#include <MiniEngine/Core/Time.h>

#include <chrono>
#include <cstdint>
#include <functional>

namespace MiniEngine
{
struct FixedStepResult
{
    std::uint32_t updateCount = 0;
    double alpha = 0.0;
    Duration acceptedElapsed{};
    bool wasClamped = false;
    bool backlogDropped = false;
};

class FixedStepScheduler
{
  private:
    Duration m_step;
    Duration m_maxFrameElapsed;
    Duration m_accumulator{};
    std::uint32_t m_maxStepsPerFrame;

  public:
    explicit FixedStepScheduler(Duration step = std::chrono::nanoseconds{16'666'667},
                                Duration maxFrameElapsed = std::chrono::milliseconds{250},
                                std::uint32_t maxStepsPerFrame = 8)
        : m_step{step}, m_maxFrameElapsed{maxFrameElapsed}, m_maxStepsPerFrame{maxStepsPerFrame}
    {
        ME_VERIFY(m_step > Duration::zero(), "step must be positive");
        ME_VERIFY(m_maxFrameElapsed >= m_step, "Max frame elapsed must be at least one step");
        ME_VERIFY(m_maxStepsPerFrame > 0, "Max steps per frame must be positive");
    }

    template <typename UpdateFunction> FixedStepResult Tick(Duration elapsed, UpdateFunction&& fixedUpdate)
    {
        if (elapsed < Duration::zero())
        {
            elapsed = Duration::zero();
        }

        FixedStepResult result;

        if (elapsed > m_maxFrameElapsed)
        {
            elapsed = m_maxFrameElapsed;
            result.wasClamped = true;
        }

        result.acceptedElapsed = elapsed;
        m_accumulator += elapsed;

        while (m_accumulator >= m_step && result.updateCount < m_maxStepsPerFrame)
        {
            std::invoke(fixedUpdate, m_step);
            m_accumulator -= m_step;
            ++result.updateCount;
        }

        if (m_accumulator >= m_step)
        {
            m_accumulator = m_accumulator % m_step;
            result.backlogDropped = true;
        }

        result.alpha = static_cast<double>(m_accumulator.count()) / static_cast<double>(m_step.count());

        return result;
    }

    void Reset() noexcept
    {
        m_accumulator = Duration::zero();
    }

    [[nodiscard]] Duration Step() const noexcept
    {
        return m_step;
    }

    [[nodiscard]] Duration Accumulator() const noexcept
    {
        return m_accumulator;
    }
};
} // namespace MiniEngine