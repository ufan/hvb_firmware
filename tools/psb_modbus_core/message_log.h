#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace psb {

enum class MessageSeverity {
    Debug,
    Info,
    Success,
    Warning,
    Error,
};

struct MessageRecord {
    uint64_t sequence = 0;
    uint64_t actionId = 0;
    MessageSeverity severity = MessageSeverity::Info;
    std::string source;
    std::string text;
    std::chrono::system_clock::time_point timestamp{};
};

class MessageCenter {
public:
    explicit MessageCenter(size_t capacity = 500);

    uint64_t beginAction(const std::string& source, const std::string& text = "");
    void publish(uint64_t actionId,
                 MessageSeverity severity,
                 const std::string& source,
                 const std::string& text);
    void clearStatus(uint64_t actionId, const std::string& source);

    std::optional<MessageRecord> currentStatus() const;
    std::vector<MessageRecord> records() const;
    void clearLog();

private:
    void appendLocked(MessageRecord record);

    size_t m_capacity;
    uint64_t m_nextSequence = 1;
    uint64_t m_nextActionId = 1;
    std::optional<MessageRecord> m_currentStatus;
    std::vector<MessageRecord> m_records;
    mutable std::mutex m_mutex;
};

} // namespace psb
