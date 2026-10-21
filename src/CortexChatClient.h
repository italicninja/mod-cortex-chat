/*
 * This file is part of the mod-cortex-chat module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#ifndef MOD_CORTEX_CHAT_CLIENT_H
#define MOD_CORTEX_CHAT_CLIENT_H

#include <atomic>
#include <string>

#include "Define.h"

// One blocking request to the LLM provider: a system prompt and one user message in, the reply text out. Worker
// threads only; nothing here touches the game.
namespace CortexChatClient
{
    enum class Provider : uint8
    {
        ANTHROPIC,  // Messages API: POST .../v1/messages, x-api-key
        OPENAI,     // Chat Completions: POST .../v1/chat/completions, Bearer key (OpenAI, OpenRouter, vLLM ...)
        OLLAMA,     // POST .../api/chat, no key
    };

    struct Settings
    {
        Provider provider = Provider::ANTHROPIC;
        std::string url;
        std::string model;
        std::string apiKey;
        uint32 maxTokens = 300;
        std::string thinking;  // Anthropic: "disabled", "adaptive", or empty to leave it out
        std::string effort;    // Anthropic: output_config.effort, or empty to leave it out
        std::string openAiMaxTokensField = "max_tokens";  // newer OpenAI models want "max_completion_tokens"
        uint32 connectTimeoutMs = 5000;
        uint32 timeoutMs = 20000;
        uint32 retries = 1;          // more tries after a 429, 5xx, 529 or connection error
        uint32 maxRetryWaitMs = 8000;
        std::string caCertFile;  // HTTPS: CA bundle; empty or missing uses OpenSSL's default paths
    };

    struct Result
    {
        bool ok = false;
        bool refused = false;  // the provider declined (Anthropic stop_reason "refusal"); no reply
        std::string text;
        std::string error;
        int status = 0;
        uint32 inputTokens = 0;
        uint32 outputTokens = 0;
        uint32 attempts = 0;
    };

    bool ParseProvider(std::string const& name, Provider& provider);
    char const* ProviderName(Provider provider);

    // Blocks for the request and its retries; `stop` cuts retry waits short at shutdown.
    Result Complete(Settings const& settings, std::string const& system, std::string const& user,
                    std::atomic<bool> const& stop);
}

#endif
