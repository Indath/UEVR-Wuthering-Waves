#include "WindowFilter.hpp"

#include <spdlog/spdlog.h>

// To prevent usage of statics (TLS breaks the present thread...?)
std::unique_ptr<WindowFilter> g_window_filter{};

WindowFilter& WindowFilter::get() {
    if (g_window_filter == nullptr) {
        g_window_filter = std::make_unique<WindowFilter>();
    }

    return *g_window_filter;
}

WindowFilter::WindowFilter() {
    // We create a job thread because GetWindowTextA can actually deadlock inside
    // the present thread...
    m_job_thread = std::make_unique<std::jthread>([this](std::stop_token s){
        while (!s.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});

            m_last_job_tick = std::chrono::steady_clock::now();

            // BUGFIX: this used to `return` here, which permanently kills this thread the first
            // time it ticks with no queued jobs (i.e. within ~100ms of startup, before any window
            // has even called is_filtered() once). Once dead, m_last_job_tick freezes forever, so
            // is_filtered()'s "job pending, trust it for up to 2s" check falls through to treating
            // *every* newly-seen window (including the game's real swapchain window) as filtered
            // on its first Present call, with nothing left alive to ever reclassify it. That first
            // Present is often the only one available during an early loading screen, so losing it
            // means Framework::initialize() never runs and the hook is wrongly declared dead.
            if (m_window_jobs.empty()) {
                continue;
            }

            std::scoped_lock _{m_mutex};

            for (const auto hwnd : m_window_jobs) {
                if (is_filtered_nocache(hwnd)) {
                    filter_window(hwnd);
                }
            }

            SPDLOG_INFO("[WindowFilter DIAG] job thread tick processed {} pending window(s)", m_window_jobs.size());

            m_window_jobs.clear();
        }
    });
}

WindowFilter::~WindowFilter() {
    m_job_thread->request_stop();
    m_job_thread->join();
}

bool WindowFilter::is_filtered(HWND hwnd) {
    if (hwnd == nullptr) {
        return true;
    }

    std::scoped_lock _{m_mutex};

    if (m_filtered_windows.find(hwnd) != m_filtered_windows.end()) {
        SPDLOG_INFO("[WindowFilter DIAG] hwnd={:x} is permanently filtered (matched is_filtered_nocache previously)", (uintptr_t)hwnd);
        return true;
    }

    // If there is a job for this window, filter it until the job is done
    if (m_window_jobs.find(hwnd) != m_window_jobs.end()) {
        // If the thread is dead for some reason, do not filter it.
        const auto job_age = std::chrono::steady_clock::now() - m_last_job_tick;
        const auto thread_alive = job_age <= std::chrono::seconds{2};
        SPDLOG_INFO("[WindowFilter DIAG] hwnd={:x} has a pending classification job (age since last job tick={}ms), filtering={}",
            (uintptr_t)hwnd, std::chrono::duration_cast<std::chrono::milliseconds>(job_age).count(), thread_alive);
        return thread_alive;
    }

    // if we havent even seen this window yet, queue it for background classification but do NOT
    // filter this Present call. Previously this returned true (filtered) here, which meant the very
    // first Present for a brand new window was always dropped on the floor while the ~100ms job
    // thread classified it. For swapchains that only present once on a given window before stalling
    // (e.g. a loading-screen swapchain belonging to the real game window), that dropped first frame
    // is the ONLY frame on_present ever gets a chance to see, so the hook never attaches even though
    // the window is later (correctly) classified as clean. Optimistically allow it through instead;
    // if the background job later proves it's a problem window, it gets added to
    // m_filtered_windows and every subsequent call will be blocked as before.
    if (m_seen_windows.find(hwnd) == m_seen_windows.end()) {
        m_seen_windows.insert(hwnd);
        m_window_jobs.insert(hwnd);
        SPDLOG_INFO("[WindowFilter DIAG] hwnd={:x} seen for the first time, queuing classification job and allowing this Present call through", (uintptr_t)hwnd);
        return false;
    }

    return false;
}

bool WindowFilter::is_filtered_nocache(HWND hwnd) {
    // get window name
    char window_name[256]{};
    GetWindowTextA(hwnd, window_name, sizeof(window_name));

    const auto sv = std::string_view{window_name};

    if (sv.find("UE4SS") != std::string_view::npos) {
        SPDLOG_INFO("[WindowFilter DIAG] hwnd={:x} (title='{}') matched UE4SS filter pattern", (uintptr_t)hwnd, sv);
        return true;
    }

    if (sv.find("PimaxXR") != std::string_view::npos) {
        SPDLOG_INFO("[WindowFilter DIAG] hwnd={:x} (title='{}') matched PimaxXR filter pattern", (uintptr_t)hwnd, sv);
        return true;
    }

    SPDLOG_INFO("[WindowFilter DIAG] hwnd={:x} (title='{}') did not match any filter pattern, will be unfiltered going forward", (uintptr_t)hwnd, sv);

    // TODO: more problematic windows
    return false;
}