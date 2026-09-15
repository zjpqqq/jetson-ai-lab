#pragma once

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <stdexcept>
#include <string>

namespace llm {

// 在 main 中创建一次，初始化 libcurl。
class CurlRuntime {
public:
    CurlRuntime() {
        const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(rc));
        }
    }

    ~CurlRuntime() {
        curl_global_cleanup();
    }

    CurlRuntime(const CurlRuntime&) = delete;
    CurlRuntime& operator=(const CurlRuntime&) = delete;
};

// libcurl 收到 HTTP 响应数据时调用。
// 返回实际接收的字节数；返回 0 表示中止接收。
inline std::size_t receive_body(
        char* data, std::size_t size,
        std::size_t count, void* userdata) noexcept {

    auto& output = *static_cast<std::string*>(userdata);
    constexpr std::size_t LIMIT = 1024 * 1024;

    // 同时防止乘法溢出和响应过大。
    if (size != 0 && count > LIMIT / size) {
        return 0;
    }

    const std::size_t bytes = size * count;

    if (bytes > LIMIT - output.size()) {
        return 0;
    }

    try {
        output.append(data, bytes);
        return bytes;
    } catch (...) {
        // C++ 异常不能穿过 libcurl 的 C 回调边界。
        return 0;
    }
}

inline std::string generate(const std::string& text) {
    using json = nlohmann::json;

    const json payload = {
        {"model", "qwen3-0.6b"},
        {"messages", json::array({
            {
                {"role", "system"},
                {"content", "请使用中文简短回答。"}
            },
            {
                {"role", "user"},
                {"content", text + "\n/no_think"}
            }
        })},
        {"max_tokens", 128},
        {"stream", false},
        {"chat_template_kwargs", {
            {"enable_thinking", false}
        }}
    };

    const std::string request_body = payload.dump();
    std::string response_body;

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>
        curl(curl_easy_init(), &curl_easy_cleanup);

    if (!curl) {
        throw std::runtime_error("无法创建 HTTP 客户端");
    }

    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>
        headers(
            curl_slist_append(nullptr, "Content-Type: application/json"),
            &curl_slist_free_all
        );

    if (!headers) {
        throw std::runtime_error("无法创建 HTTP 请求头");
    }

    // 检查每个选项是否设置成功。
    auto option = [&](CURLoption key, auto value) {
        const CURLcode rc = curl_easy_setopt(curl.get(), key, value);
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

    // 本机推理不经过 Clash 等代理。
    option(CURLOPT_NOPROXY, "*");

    option(CURLOPT_CONNECTTIMEOUT, 5L);
    option(CURLOPT_TIMEOUT, 120L);
    option(CURLOPT_NOSIGNAL, 1L);

    option(CURLOPT_WRITEFUNCTION, &receive_body);
    option(CURLOPT_WRITEDATA, &response_body);

    const CURLcode rc = curl_easy_perform(curl.get());

    if (rc != CURLE_OK) {
        throw std::runtime_error(
            std::string("模型 HTTP 请求失败：")
            + curl_easy_strerror(rc));
    }

    long status = 0;
    const CURLcode info_rc =
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);

    if (info_rc != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(info_rc));
    }

    if (status < 200 || status >= 300) {
        throw std::runtime_error(
            "模型返回 HTTP " + std::to_string(status)
            + "：" + response_body.substr(0, 300));
    }

    const json result = json::parse(response_body);
    const auto& content =
        result.at("choices").at(0).at("message").at("content");

    if (!content.is_string()) {
        throw std::runtime_error("模型响应中没有文本 content");
    }

    const std::string answer = content.get<std::string>();

    if (answer.empty()) {
        throw std::runtime_error("模型返回了空回答");
    }

    return answer;
}

} // namespace llm