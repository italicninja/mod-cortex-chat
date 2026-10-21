/*
 * This file is part of the mod-cortex-chat module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "Channel.h"
#include "CortexChat.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "PlayerbotsExtensions.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldScript.h"

namespace
{
    // Real players only: bots' own chat never asks the LLM, nor does addon traffic.
    bool FromPlayer(Player* player, uint32 lang) { return lang != LANG_ADDON && IsRealPlayer(player); }

    class CortexChatPlayerScript : public PlayerScript
    {
    public:
        CortexChatPlayerScript()
            : PlayerScript("CortexChatPlayerScript",
                           {PLAYERHOOK_CAN_PLAYER_USE_CHAT, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
                            PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT, PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
                            PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT})
        {
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg) override
        {
            if (type == CHAT_MSG_SAY && FromPlayer(player, lang))
                CortexChat::OnSay(player, msg);
            return true;
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
        {
            if (type == CHAT_MSG_WHISPER && receiver && FromPlayer(player, lang))
                CortexChat::OnWhisper(player, receiver, msg);
            return true;
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override
        {
            if ((type == CHAT_MSG_PARTY || type == CHAT_MSG_PARTY_LEADER || type == CHAT_MSG_RAID ||
                 type == CHAT_MSG_RAID_LEADER) &&
                FromPlayer(player, lang))
                CortexChat::OnParty(player, group, msg);
            return true;
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Guild* /*guild*/) override
        {
            if (type == CHAT_MSG_GUILD && FromPlayer(player, lang))
                CortexChat::OnGuild(player, msg);
            return true;
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Channel* channel) override
        {
            if (type == CHAT_MSG_CHANNEL && FromPlayer(player, lang))
                CortexChat::OnChannel(player, channel, msg);
            return true;
        }
    };

    class CortexChatWorldScript : public WorldScript
    {
    public:
        CortexChatWorldScript()
            : WorldScript("CortexChatWorldScript", {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP,
                                                    WORLDHOOK_ON_SHUTDOWN, WORLDHOOK_ON_UPDATE})
        {
        }

        void OnAfterConfigLoad(bool /*reload*/) override { CortexChat::LoadConfig(); }
        void OnStartup() override { CortexChat::Start(); }
        void OnShutdown() override { CortexChat::Stop(); }
        void OnUpdate(uint32 /*diff*/) override { CortexChat::Update(); }
    };
}

void AddCortexChatScripts()
{
    new CortexChatPlayerScript();
    new CortexChatWorldScript();
    PlayerbotsExtensions::AddChatFilter({CortexChat::Claims, CortexChat::Silences});
}
