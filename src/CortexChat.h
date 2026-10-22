/*
 * This file is part of the mod-cortex-chat module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#ifndef MOD_CORTEX_CHAT_H
#define MOD_CORTEX_CHAT_H

#include <set>
#include <string>

#include "CortexChatClient.h"
#include "Define.h"

class Channel;
class Group;
class Player;

// Cortex chat: bots answer real players' chat with an LLM (whispers, /say, party, guild, General), in character and
// from what the bot is actually doing. Chat hooks pick the bot that answers and build the prompt on the world thread;
// worker threads make the HTTP requests; replies go out on the world thread after a typing delay.
namespace CortexChat
{
    enum class Kind : uint8
    {
        WHISPER,
        SAY,
        PARTY,
        GUILD,
        CHANNEL,
    };

    struct Config
    {
        bool enabled = false;
        CortexChatClient::Settings client;
        uint32 workers = 2;
        uint32 queueSize = 32;
        uint32 maxPerMinute = 20;
        uint32 maxPerDay = 3000;
        uint32 playerCooldownMs = 1500;
        uint32 maxReplyAgeMs = 45000;

        uint32 readDelayMs = 1500;
        uint32 typingMsPerChar = 50;
        uint32 maxTypingMs = 10000;
        uint32 maxReplyLength = 250;

        bool whisper = true;
        bool say = true;
        float sayRange = 25.0f;
        uint32 sayChance = 50;
        bool party = true;
        uint32 partyChance = 70;
        bool guild = true;
        uint32 guildChance = 40;
        bool channel = true;
        std::set<uint32> channelIds = {1};
        uint32 channelQuestionChance = 70;
        uint32 channelChance = 10;

        uint32 historyLines = 8;
        uint32 historyMinutes = 20;
        std::string extraInstructions;
        std::string systemPrompt;  // CortexChatPrompt::System with the extra instructions
        bool logPrompts = false;
    };

    // World thread: config (re)load, start and stop of the workers, and the per-tick delivery.
    void LoadConfig();
    void Start();
    void Stop();
    void Update();

    // Chat hooks, world thread: `from` is a real player.
    void OnWhisper(Player* from, Player* bot, std::string const& msg);
    void OnSay(Player* from, std::string const& msg);
    void OnParty(Player* from, Group* group, std::string const& msg);
    void OnGuild(Player* from, std::string const& msg);
    void OnChannel(Player* from, Channel* channel, std::string const& msg);

    // CortexChatHook::ChatFilter (mod-cortex/src/Body/Script/CortexChatHook.h).
    bool Claims(Player* bot, Player* from, uint32 type, std::string const& msg);
    bool Silences(Player* bot, uint32 type);
}

#endif
