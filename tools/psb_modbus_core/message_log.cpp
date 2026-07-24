#include "message_log.h"

#include <algorithm>
#include <utility>

namespace psb {

MessageCenter::MessageCenter(size_t capacity)
    : m_capacity(std::max<size_t>(capacity, 1)) {}

uint64_t MessageCenter::beginAction(const std::string& source, const std::string& text) {
    std::lock_guard<std::mutex> lk(m_mutex);
    uint64_t actionId = m_nextActionId++;
    m_currentStatus.reset();
    if (!text.empty()) {
        MessageRecord record;
        record.actionId = actionId;
        record.severity = MessageSeverity::Info;
        record.source = source;
        record.text = text;
        record.timestamp = std::chrono::system_clock::now();
        appendLocked(std::move(record));
        m_currentStatus = m_records.back();
    }
    return actionId;
}

void MessageCenter::publish(uint64_t actionId,
                            MessageSeverity severity,
                            const std::string& source,
                            const std::string& text) {
    std::lock_guard<std::mutex> lk(m_mutex);
    MessageRecord record;
    record.actionId = actionId;
    record.severity = severity;
    record.source = source;
    record.text = text;
    record.timestamp = std::chrono::system_clock::now();
    appendLocked(std::move(record));
    m_currentStatus = m_records.back();
}

void MessageCenter::clearStatus(uint64_t actionId, const std::string& source) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_currentStatus && m_currentStatus->actionId == actionId &&
        m_currentStatus->source == source) {
        m_currentStatus.reset();
    }
}

std::optional<MessageRecord> MessageCenter::currentStatus() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_currentStatus;
}

std::vector<MessageRecord> MessageCenter::records() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_records;
}

void MessageCenter::clearLog() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_records.clear();
}

void MessageCenter::appendLocked(MessageRecord record) {
    record.sequence = m_nextSequence++;
    m_records.push_back(std::move(record));
    while (m_records.size() > m_capacity)
        m_records.erase(m_records.begin());
}

} // namespace psb
