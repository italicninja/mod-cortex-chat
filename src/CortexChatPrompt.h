/*
 * This file is part of the mod-cortex-chat module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

#ifndef MOD_CORTEX_CHAT_PROMPT_H
#define MOD_CORTEX_CHAT_PROMPT_H

#include <string>

class Player;

// Prompt text from game state. World thread, while maps aren't updating (chat handling).
namespace CortexChatPrompt
{
    // Persona descriptions from cortex_personas (playerbots DB); at startup.
    void LoadPersonas();

    // The fixed rules, plus the configured extra instructions.
    std::string System(std::string const& extra);

    // Who the bot is, where, and what it's doing.
    std::string AboutBot(Player* bot);

    // The speaker as the bot sees them.
    std::string AboutSpeaker(Player* from, Player* bot);

    // What the model wrote, made fit for game chat: one line, ASCII, no '|' escapes, no "Name:" prefix. Empty when
    // the model chose not to answer ("[skip]").
    std::string Clean(std::string text, std::string const& botName);
}

#endif
