#pragma once

#include "llm_http.hpp"

#include <exception>
#include <functional>
#include <utility>

namespace llm {

// 只解析当前聊天接口需要的 SSE 数据。
// 不提供浏览器 EventSource 的自动重连等功能。
class SseParser {
public:
    explicit SseParser(
        std::function<void(const std::string&)> on_delta)
        : on_delta_(std::move(on_delta)) {}

    void feed(const char* bytes, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            const char ch = bytes[i];

            // 同时兼容 LF、CRLF 和 CR 换行。
            if (skip_lf_) {
                skip_lf_ = false;
                if (ch == '\n') {
                    continue;
                }
            }

            if (ch == '\r' || ch == '\n') {
                process_line();
                line_.clear();
                skip_lf_ = (ch == '\r');
            } else {
                if (line_.size() >= LIMIT) {
                    throw std::runtime_error("SSE 行过长");
                }
                line_.push_back(ch);
            }
        }
    }

    bool done() const {
        return done_;
    }

    const std::string& finish_reason() const {
        return finish_reason_;
    }

private:
    static constexpr std::size_t LIMIT = 1024 * 1024;

    std::string line_;
    std::string event_data_;
    std::string finish_reason_ = "unknown";

    bool skip_lf_ = false;
    bool done_ = false;

    std::function<void(const std::string&)> on_delta_;

    void process_line() {
        if (line_.empty()) {
            // 空行表示一个 SSE 事件结束。
            if (!event_data_.empty()) {
                std::string data = std::move(event_data_);
                event_data_.clear();

                // 去掉拼接 data 行时增加的最后一个换行。
                data.pop_back();
                process_event(data);
            }
            return;
        }

        // SSE 注释行。
        if (line_[0] == ':') {
            return;
        }

        const auto colon = line_.find(':');
        const std::string field = line_.substr(0, colon);

        if (field != "data") {
            return;
        }

        std::string value;

        if (colon != std::string::npos) {
            value = line_.substr(colon + 1);

            if (!value.empty() && value.front() == ' ') {
                value.erase(0, 1);
            }
        }

        if (event_data_.size() + value.size() + 1 > LIMIT) {
            throw std::runtime_error("SSE 事件过大");
        }

        // 一个事件可能含有多行 data。
        event_data_ += value;
        event_data_ += '\n';
    }

    void process_event(const std::string& data) {
        if (done_) {
            return;
        }

        if (data == "[DONE]") {
            done_ = true;
            return;
        }

        if (data.empty()) {
            return;
        }

        const auto event = nlohmann::json::parse(data);

        if (!event.is_object()) {
            throw std::runtime_error("SSE 数据不是 JSON 对象");
        }

        if (event.contains("error")) {
            throw std::runtime_error(
                "模型流返回错误：" + event.at("error").dump());
        }

        if (!event.contains("choices")) {
            return;
        }

        const auto& choices = event.at("choices");

        if (!choices.is_array()) {
            throw std::runtime_error("choices 不是数组");
        }

        // 例如某些只包含统计信息的事件。
        if (choices.empty()) {
            return;
        }

        const auto& choice = choices.at(0);

        if (choice.contains("finish_reason")
            && choice.at("finish_reason").is_string()) {
            finish_reason_ =
                choice.at("finish_reason").get<std::string>();
        }

        if (!choice.contains("delta")
            || !choice.at("delta").is_object()) {
            return;
        }

        const auto& delta = choice.at("delta");

        // 角色、reasoning_content 等不是本示例要显示的正文。
        if (!delta.contains("content")
            || delta.at("content").is_null()) {
            return;
        }

        const std::string text =
            delta.at("content").get<std::string>();

        if (!text.empty()) {
            on_delta_(text);
        }
    }
};

struct StreamContext {
    CURL* curl;
    SseParser* parser;
    std::exception_ptr error;
};

// 不能让 C++ 异常穿过 libcurl 的 C 回调边界。
inline std::size_t receive_stream(
    char* data, std::size_t size,
    std::size_t count, void* userdata) noexcept {

    auto& context = *static_cast<StreamContext*>(userdata);

    try {
        constexpr std::size_t LIMIT = 1024 * 1024;

        if (size != 0 && count > LIMIT / size) {
            throw std::runtime_error("HTTP 数据块过大");
        }

        const std::size_t bytes = size * count;

        long status = 0;
        const CURLcode rc = curl_easy_getinfo(
            context.curl, CURLINFO_RESPONSE_CODE, &status);

        if (rc != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(rc));
        }

        // HTTP 错误响应未必是 SSE，不送进解析器。
        if (status >= 200 && status < 300) {
            context.parser->feed(data, bytes);
        }

        return bytes;
    } catch (...) {
        context.error = std::current_exception();
        return 0;  // 让 libcurl 中止本次传输。
    }
}

// 每收到一个正文片段就调用 on_delta。
// 返回 stop / length 等结束原因。
inline std::string generate_stream(
    const std::string& text,
    const std::function<void(const std::string&)>& on_delta) {

    using json = nlohmann::json;

    const json payload = {
        {"model", "qwen3-0.6b"},
        {"messages", json::array({
            {
                {"role", "user"},
                {"content", text + "\n/no_think"}
            }
        })},
        {"max_tokens", 256},
        {"stream", true},
        {"chat_template_kwargs", {
            {"enable_thinking", false}
        }}
    };

    const std::string request_body = payload.dump();
    SseParser parser(on_delta);

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>
        curl(curl_easy_init(), &curl_easy_cleanup);

    if (!curl) {
        throw std::runtime_error("无法创建 HTTP 客户端");
    }

    StreamContext context{curl.get(), &parser, nullptr};

    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>
        headers(
            curl_slist_append(
                nullptr, "Content-Type: application/json"),
            &curl_slist_free_all
        );

    if (!headers) {
        throw std::runtime_error("无法创建 HTTP 请求头");
    }

    auto option = [&](CURLoption key, auto value) {
        const CURLcode rc =
            curl_easy_setopt(curl.get(), key, value);

        if (rc != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(rc));
        }
    };

    option(CURLOPT_URL,
           "http://127.0.0.1:8080/v1/chat/completions");
    option(CURLOPT_HTTPHEADER, headers.get());
    option(CURLOPT_POST, 1L);
    option(CURLOPT_POSTFIELDS, request_body.c_str());
    option(CURLOPT_POSTFIELDSIZE,
           static_cast<long>(request_body.size()));

    option(CURLOPT_NOPROXY, "*");
    option(CURLOPT_CONNECTTIMEOUT, 5L);
    option(CURLOPT_TIMEOUT, 120L);
    option(CURLOPT_NOSIGNAL, 1L);

    option(CURLOPT_WRITEFUNCTION, &receive_stream);
    option(CURLOPT_WRITEDATA, &context);

    const CURLcode rc = curl_easy_perform(curl.get());

    // 优先报告解析或 TCP 转发中的原始错误。
    if (context.error) {
        std::rethrow_exception(context.error);
    }

    if (rc != CURLE_OK) {
        throw std::runtime_error(
            std::string("模型 HTTP 请求失败：")
            + curl_easy_strerror(rc));
    }

    long status = 0;
    const CURLcode info_rc = curl_easy_getinfo(
        curl.get(), CURLINFO_RESPONSE_CODE, &status);

    if (info_rc != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(info_rc));
    }

    if (status < 200 || status >= 300) {
        throw std::runtime_error(
            "模型返回 HTTP " + std::to_string(status));
    }

    if (!parser.done()) {
        throw std::runtime_error(
            "HTTP 流已结束，但没有收到 [DONE]");
    }

    return parser.finish_reason();
}

} // namespace llm