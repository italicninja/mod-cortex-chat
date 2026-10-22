/*
 * This file is part of the mod-cortex-chat module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#include "Chat.h"
#include "Player.h"
#include "PlayerScript.h"
#include "WorldSession.h"

void AddCortexChatScripts();

// The version this source is tagged with in https://github.com/italicninja/mod-cortex-chat (bump it with each tag).
constexpr char const* CORTEXCHAT_VERSION = "v0.2.0";

namespace
{
    // Tells each real player at login that the realm runs this module, like mod-cortex does. GPL-2.0: the link
    // must lead to the module's public source.
    class CortexChatLoginScript : public PlayerScript
    {
    public:
        CortexChatLoginScript() : PlayerScript("CortexChatLoginScript", {PLAYERHOOK_ON_LOGIN}) { }

        void OnPlayerLogin(Player* player) override
        {
            if (player->GetSession()->IsHeadless())
                return;
            std::string line = "|cff00ff00This server runs with |cff00ccffmod-cortex-chat|r |cffffd100";
            line += CORTEXCHAT_VERSION;
            line += "|r |cffcccccchttps://github.com/italicninja/mod-cortex-chat|r";
            ChatHandler(player->GetSession()).SendSysMessage(line);
        }
    };
}

// Called by the core's module loader (Add<module name with '-' as '_'>Scripts).
void Addmod_cortex_chatScripts()
{
    AddCortexChatScripts();
    new CortexChatLoginScript();
}
