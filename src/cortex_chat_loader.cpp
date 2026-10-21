/*
 * This file is part of the mod-cortex-chat module for AzerothCore, released under GNU GPL v2 license,
 * redistribute/modify under version 2 of the License, or (at your option) any later version.
 */

void AddCortexChatScripts();

// Called by the core's module loader (Add<module name with '-' as '_'>Scripts).
void Addmod_cortex_chatScripts() { AddCortexChatScripts(); }
