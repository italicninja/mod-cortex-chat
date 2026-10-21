/*
 * This file is part of the mod-cortex-chat module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "CortexChatClient.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <thread>

// The only file that includes the vendored libraries (deps/README.md); HTTPS over the OpenSSL the core links.
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "../deps/httplib.h"
#include "../deps/json.hpp"

using json = nlohmann::json;

namespace
{
    struct Endpoint
    {
        std::string base;  // scheme://host[:port]
        std::string path;
    };

    bool SplitUrl(std::string const& url, Endpoint& endpoint)
    {
        std::size_t scheme = url.find("://");
        if (scheme == std::string::npos)
            return false;
        std::size_t slash = url.find('/', scheme + 3);
        endpoint.base = url.substr(0, slash);
        endpoint.path = slash == std::string::npos ? "/" : url.substr(slash);
        return endpoint.base.size() > scheme + 3;
    }

    json Body(CortexChatClient::Settings const& s, std::string const& system, std::string const& user)
    {
        using CortexChatClient::Provider;
        switch (s.provider)
        {
            case Provider::ANTHROPIC:
            {
                json body = {
                    {"model", s.model},
                    {"max_tokens", s.maxTokens},
                    {"system", system},
                    {"messages", json::array({{{"role", "user"}, {"content", user}}})},
                };
                if (!s.thinking.empty())
                    body["thinking"] = {{"type", s.thinking}};
                if (!s.effort.empty())
                    body["output_config"] = {{"effort", s.effort}};
                return body;
            }
            case Provider::OPENAI:
            {
                json body = {
                    {"model", s.model},
                    {"messages",
                     json::array({{{"role", "system"}, {"content", system}}, {{"role", "user"}, {"content", user}}})},
                };
                body[s.openAiMaxTokensField] = s.maxTokens;
                return body;
            }
            case Provider::OLLAMA:
            default:
                return {
                    {"model", s.model},
                    {"stream", false},
                    {"think", false},
                    {"options", {{"num_predict", s.maxTokens}}},
                    {"messages",
                     json::array({{{"role", "system"}, {"content", system}}, {{"role", "user"}, {"content", user}}})},
                };
        }
    }

    httplib::Headers Headers(CortexChatClient::Settings const& s)
    {
        using CortexChatClient::Provider;
        httplib::Headers headers;
        if (s.provider == Provider::ANTHROPIC)
        {
            headers.emplace("anthropic-version", "2023-06-01");
            if (!s.apiKey.empty())
                headers.emplace("x-api-key", s.apiKey);
        }
        else if (!s.apiKey.empty())
            headers.emplace("Authorization", "Bearer " + s.apiKey);
        return headers;
    }

    // The reply text and token counts from a 200 response.
    void Parse(CortexChatClient::Settings const& s, std::string const& body, CortexChatClient::Result& result)
    {
        using CortexChatClient::Provider;
        json doc = json::parse(body, nullptr, false);
        if (doc.is_discarded() || !doc.is_object())
        {
            result.error = "response is not JSON";
            return;
        }

        switch (s.provider)
        {
            case Provider::ANTHROPIC:
                if (doc.value("stop_reason", "") == "refusal")
                {
                    result.refused = true;
                    result.error = "refusal";
                    return;
                }
                if (doc.contains("content") && doc["content"].is_array())
                    for (json const& block : doc["content"])
                        if (block.value("type", "") == "text")
                            result.text += block.value("text", "");
                if (doc.contains("usage") && doc["usage"].is_object())
                {
                    result.inputTokens = doc["usage"].value("input_tokens", 0u);
                    result.outputTokens = doc["usage"].value("output_tokens", 0u);
                }
                break;
            case Provider::OPENAI:
                if (doc.contains("choices") && doc["choices"].is_array() && !doc["choices"].empty())
                {
                    json const& message = doc["choices"][0]["message"];
                    if (message.is_object() && message.contains("content") && message["content"].is_string())
                        result.text = message["content"].get<std::string>();
                }
                if (doc.contains("usage") && doc["usage"].is_object())
                {
                    result.inputTokens = doc["usage"].value("prompt_tokens", 0u);
                    result.outputTokens = doc["usage"].value("completion_tokens", 0u);
                }
                break;
            case Provider::OLLAMA:
                if (doc.contains("message") && doc["message"].is_object())
                    result.text = doc["message"].value("content", "");
                result.inputTokens = doc.value("prompt_eval_count", 0u);
                result.outputTokens = doc.value("eval_count", 0u);
                break;
        }
        result.ok = true;
    }

    bool Retryable(int status) { return status == 408 || status == 429 || status >= 500; }

    // Sleeps in short steps so shutdown doesn't wait out a long retry-after.
    void Wait(uint32 ms, std::atomic<bool> const& stop)
    {
        for (uint32 waited = 0; waited < ms && !stop.load(std::memory_order_relaxed); waited += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool CortexChatClient::ParseProvider(std::string const& name, Provider& provider)
{
    if (name == "anthropic")
        provider = Provider::ANTHROPIC;
    else if (name == "openai")
        provider = Provider::OPENAI;
    else if (name == "ollama")
        provider = Provider::OLLAMA;
    else
        return false;
    return true;
}

char const* CortexChatClient::ProviderName(Provider provider)
{
    switch (provider)
    {
        case Provider::ANTHROPIC:
            return "anthropic";
        case Provider::OPENAI:
            return "openai";
        default:
            return "ollama";
    }
}

CortexChatClient::Result CortexChatClient::Complete(Settings const& s, std::string const& system,
                                                    std::string const& user, std::atomic<bool> const& stop)
{
    Result result;
    Endpoint endpoint;
    if (!SplitUrl(s.url, endpoint))
    {
        result.error = "bad url";
        return result;
    }

    httplib::Client client(endpoint.base);
    if (!client.is_valid())
    {
        result.error = "unsupported url (https needs OpenSSL)";
        return result;
    }
    client.set_connection_timeout(s.connectTimeoutMs / 1000, (s.connectTimeoutMs % 1000) * 1000);
    client.set_read_timeout(s.timeoutMs / 1000, (s.timeoutMs % 1000) * 1000);
    client.set_write_timeout(s.timeoutMs / 1000, (s.timeoutMs % 1000) * 1000);
    std::error_code ec;
    if (!s.caCertFile.empty() && std::filesystem::exists(s.caCertFile, ec))
        client.set_ca_cert_path(s.caCertFile);

    std::string const body = Body(s, system, user).dump();
    httplib::Headers const headers = Headers(s);

    for (uint32 attempt = 0; attempt <= s.retries && !stop.load(std::memory_order_relaxed); ++attempt)
    {
        result = Result();
        result.attempts = attempt + 1;
        httplib::Result res = client.Post(endpoint.path, headers, body, "application/json");
        uint32 waitMs = 1000 * (attempt + 1);
        if (!res)
            result.error = httplib::to_string(res.error());
        else
        {
            result.status = res->status;
            if (res->status == 200)
            {
                Parse(s, res->body, result);
                return result;
            }
            // The provider's own message says what's wrong (bad key, unknown model ...); keep it short for the log.
            result.error = "HTTP " + std::to_string(res->status) + ": " + res->body.substr(0, 300);
            if (!Retryable(res->status))
                return result;
            std::string retryAfter = res->get_header_value("retry-after");
            if (!retryAfter.empty() && std::isdigit(static_cast<unsigned char>(retryAfter[0])))
                waitMs = static_cast<uint32>(std::min<unsigned long>(std::stoul(retryAfter), 3600)) * 1000;
        }
        if (attempt < s.retries)
        {
            if (waitMs > s.maxRetryWaitMs)
                return result;  // a reply that late is no use in chat
            Wait(waitMs, stop);
        }
    }
    return result;
}
