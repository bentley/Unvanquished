/*
===========================================================================

Daemon GPL Source Code
Copyright (C) 2013 Unvanquished Developers

This file is part of the Daemon GPL Source Code (Daemon Source Code).

Daemon Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Daemon Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Daemon Source Code.  If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/

#include "CvarSystem.h"
#include "CommandSystem.h"
#include "../../common/String.h"
#include "../../common/Log.h"
#include "../../common/Command.h"

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../qcommon/cvar.h"

#include <unordered_map>

//TODO: thread safety (not possible with the C API that doesn't care at all about this)

namespace Cvar {

    //The server gives the sv_cheats cvar to the client, on 'off' it prevents the user from changing Cvar::CHEAT cvars
    void SetCheatMode(bool cheats);
    Callback<Cvar<bool>> cvar_cheats("sv_cheats", "can cheats be used in the current game", SYSTEMINFO | ROM, true, SetCheatMode);

    // A cvar is some info alongside a potential proxy for the cvar
    struct cvarRecord_t {
        std::string value;
        std::string resetValue;
        int flags;
        std::string description;
        CvarProxy* proxy;
        cvar_t ccvar; // The state of the cvar_t used to emulate the C API
        //DO: mutex?
    };

    //Functions that emulate the C API
    void SetCStyleDescription(cvarRecord_t& cvar) {
        if (cvar.proxy) {
            return;
        }

        if (cvar.flags & CVAR_LATCH and cvar.ccvar.latchedString) {
            cvar.description = Str::Format("\"%s\", latched value \"%s\"", cvar.ccvar.string, cvar.ccvar.latchedString);
        } else {
            cvar.description = Str::Format("\"%s\"", cvar.value);
        }

        Cmd::ChangeDescription(cvar.ccvar.name, cvar.description);
    }

    void SetCCvar(cvarRecord_t& cvar) {
        cvar_t& var = cvar.ccvar;
        bool modified = false;

        if (cvar.flags & CVAR_LATCH) {
            if (not var.string) {
                var.string = CopyString(cvar.value.c_str());
                modified = true;
            } else {
                if (Q_stricmp(var.string, cvar.value.c_str())) {
                    Com_Printf(_("The change will take effect after restart."));
                    if (var.latchedString) Z_Free(var.latchedString);
                    var.latchedString = CopyString(cvar.value.c_str());
                } else if (var.latchedString) {
                    Z_Free(var.latchedString);
                    var.latchedString = nullptr;
                }
            }
        } else {
            if (var.string) {
                if (Q_stricmp(var.string, cvar.value.c_str())) {
                    modified = true;
                    Z_Free(var.string);
                    var.string = CopyString(cvar.value.c_str());
                }
            } else {
                var.string = CopyString(cvar.value.c_str());
                modified = true;
            }
        }

        var.modified = modified;
        var.modificationCount++;
        var.value = atof(var.string);
        var.integer = atoi(var.string);

        if (modified) {
            cvar_modifiedFlags |= var.flags;
        }
        SetCStyleDescription(cvar);
    }

    void GetCCvar(const std::string& name, cvarRecord_t& cvar) {
        cvar_t& var = cvar.ccvar;
        bool modified = false;

        if (not var.name) {
            var.name = CopyString(name.c_str());
            var.index = -1;
            var.modificationCount = -1;
        }

        if (var.resetString) Z_Free(var.resetString);
        var.resetString = CopyString(cvar.resetValue.c_str());

        var.flags = cvar.flags;

        if (cvar.flags & CVAR_LATCH and var.latchedString) {
            if (var.string) Z_Free(var.string);
            var.string = var.latchedString;
            var.latchedString = nullptr;
            modified = true;
        } else {
            if (var.string) {
                if (Q_stricmp(var.string, cvar.value.c_str())) {
                    modified = true;
                    Z_Free(var.string);
                    var.string = CopyString(cvar.value.c_str());
                }
            } else {
                var.string = CopyString(cvar.value.c_str());
                modified = true;
            }
        }

        var.modified |= modified;
        var.modificationCount++;
        var.value = atof(var.string);
        var.integer = atoi(var.string);

        if (modified) {
            cvar_modifiedFlags |= var.flags;
        }
        SetCStyleDescription(cvar);
    }

    typedef std::unordered_map<std::string, cvarRecord_t*, Str::IHash, Str::IEqual> CvarMap;

    // The order in which static global variables are initialized is undefined and cvar
    // can be registered before main. The first time this function is called the cvar map
    // is initialized so we are sure it is initialized as soon as we need it.
    CvarMap& GetCvarMap() {
        static CvarMap* cvars = new CvarMap();
        return *cvars;
    }

    // A command created for each cvar, used for /<cvar>
    class CvarCommand : public Cmd::CmdBase {
        public:
            CvarCommand() : Cmd::CmdBase(Cmd::CVAR) {
            }

            void Run(const Cmd::Args& args) const OVERRIDE {
                CvarMap& cvars = GetCvarMap();
                const std::string& name = args.Argv(0);
                cvarRecord_t* var = cvars[name];

                if (args.Argc() < 2) {
                    Print(_("\"%s\" - %s^7 default: \"%s^7\""), name.c_str(), var->description.c_str(), var->resetValue.c_str());
                } else {
                    //TODO forward the print part of the environment
                    SetValue(name, args.Argv(1));
                }
            }
    };
    static CvarCommand cvarCommand;

    void InternalSetValue(const std::string& cvarName, std::string value, int flags, bool rom, bool warnRom) {
        CvarMap& cvars = GetCvarMap();

        auto it = cvars.find(cvarName);
        //TODO: rom means the cvar should have been created before?
        if (it == cvars.end()) {
            if (!Cmd::IsValidCvarName(cvarName)) {
                Com_Printf(_("Invalid cvar name '%s'"), cvarName.c_str());
                return;
            }

            //The user creates a new cvar through a command.
            cvarRecord_t cvar{value, value, flags | CVAR_USER_CREATED, "user created", nullptr, {}};
            cvars[cvarName] = new cvarRecord_t(std::move(cvar));
            Cmd::AddCommand(cvarName, cvarCommand, "cvar - user created");
            GetCCvar(cvarName, *cvars[cvarName]);

        } else {
            cvarRecord_t* cvar = it->second;

            if (cvar->flags & (CVAR_ROM | CVAR_INIT) and not rom) {
                Com_Printf(_("%s is read only.\n"), cvarName.c_str());
                return;
            }

            if (rom and warnRom and not (cvar->flags & (CVAR_ROM | CVAR_INIT))) {
                Com_Printf("SetValueROM called on non-ROM cvar '%s'\n", cvarName.c_str());
            }

            if (not *cvar_cheats && cvar->flags & CHEAT) {
                Com_Printf(_("%s is cheat-protected.\n"), cvarName.c_str());
                return;
            }

            std::string oldValue = std::move(cvar->value);
            cvar->value = std::move(value);
            cvar->flags |= flags;

            if (cvar->proxy) {
                //Tell the cvar proxy about the new value
                OnValueChangedResult result = cvar->proxy->OnValueChanged(cvar->value);

                if (result.success) {
                    Cmd::ChangeDescription(cvarName, result.description);
                    cvar->description = std::move(result.description);
                } else {
                    //The proxy could not parse the value, rollback
                    Com_Printf("Value '%s' is not valid for cvar %s: %s\n",
                            cvar->value.c_str(), cvarName.c_str(), result.description.c_str());
                    cvar->value = std::move(oldValue);
                }
            }
            SetCCvar(*cvar);
        }

    }

    // Simple proxies for SetValueInternal
    void SetValue(const std::string& cvarName, std::string value) {
        InternalSetValue(cvarName, std::move(value), 0, false, true);
    }

    void SetValueROM(const std::string& cvarName, std::string value) {
        InternalSetValue(cvarName, std::move(value), 0, true, true);
    }

    std::string GetValue(const std::string& cvarName) {
        CvarMap& cvars = GetCvarMap();
        std::string result = "";

        auto iter = cvars.find(cvarName);
        if (iter != cvars.end()) {
            result = iter->second->value;
        }

        return result;
    }

    void Register(CvarProxy* proxy, const std::string& name, std::string description, int flags, const std::string& defaultValue) {
        CvarMap& cvars = GetCvarMap();
        cvarRecord_t* cvar;

        auto it = cvars.find(name);
        if (it == cvars.end()) {
            if (!Cmd::IsValidCvarName(name)) {
                Com_Printf(_("Invalid cvar name '%s'"), name.c_str());
                return;
            }

            //Create the cvar and parse its default value
            cvarRecord_t temp{defaultValue, defaultValue, flags, "", proxy, {}};
            cvar = new cvarRecord_t(std::move(temp));
            cvars[name] = cvar;

            Cmd::AddCommand(name, cvarCommand, "cvar - " + description);

        } else {
            cvar = it->second;

            if (cvar->proxy) {
                Com_Printf(_("Cvar %s cannot be registered twice\n"), name.c_str());
            }

            //Register the cvar with the previous user_created value
            cvar->flags &= ~CVAR_USER_CREATED;
            cvar->flags |= flags;

            cvar->resetValue = std::move(defaultValue);
            cvar->description = "";

            /*
            if (cvar->flags & CVAR_ROM) {
                cvar->value = cvar->resetValue;
            }
            */

            if (proxy) { //TODO replace me with an assert once we do not need to support the C API
                OnValueChangedResult result = proxy->OnValueChanged(cvar->value);
                if (result.success) {
                    Cmd::ChangeDescription(name, result.description);
                    cvar->description = std::move(result.description);
                } else {
                    OnValueChangedResult defaultValueResult = proxy->OnValueChanged(defaultValue);
                    if (defaultValueResult.success) {
                        Cmd::ChangeDescription(name, result.description);
                        cvar->description = std::move(result.description);
                    } else {
                        Com_Printf(_("Default value '%s' is not correct for cvar '%s': %s\n"),
                                defaultValue.c_str(), name.c_str(), defaultValueResult.description.c_str());
                    }
                }
            }
        }
        GetCCvar(name, *cvar);
    }

    void Unregister(const std::string& cvarName) {
        CvarMap& cvars = GetCvarMap();

        auto it = cvars.find(cvarName);
        if (it != cvars.end()) {
            cvarRecord_t* cvar = it->second;

            cvar->proxy = nullptr;
            cvar->flags |= CVAR_USER_CREATED;
        } //TODO else what?
    }

    Cmd::CompletionResult Complete(Str::StringRef prefix) {
        CvarMap& cvars = GetCvarMap();

        Cmd::CompletionResult res;
        for (auto it : cvars) {
            if (Str::IsIPrefix(prefix, it.first)) {
                //TODO what do we show in the completion?
                res.push_back(std::make_pair(it.first, it.second->description));
            }
        }

        return res;
    }

    void AddFlags(const std::string& cvarName, int flags) {
        CvarMap& cvars = GetCvarMap();

        auto it = cvars.find(cvarName);
        if (it != cvars.end()) {
            cvarRecord_t* cvar = it->second;
            cvar->flags |= flags;

            //TODO: remove it, overkill ?
            //Make sure to trigger the event as if this variable was changed
            cvar_modifiedFlags |= flags;
        } //TODO else what?
    }

    void SetCheatMode(bool cheats) {
        if (not cheats) {
            return;
        }

        CvarMap& cvars = GetCvarMap();

        // Reset all the CHEAT cvars to their default value
        for (auto it : cvars) {
            cvarRecord_t* cvar = it.second;

            if (cvar->flags & CHEAT && cvar->value != cvar->resetValue) {
                cvar->value = cvar->resetValue;
                SetCCvar(*cvar);

                if (cvar->proxy) {
                    OnValueChangedResult result = cvar->proxy->OnValueChanged(cvar->resetValue);
                    if(result.success) {
                        Cmd::ChangeDescription(it.first, result.description);
                        cvar->description = std::move(result.description);
                    } else {
                        Com_Printf(_("Default value '%s' is not correct for cvar '%s': %s\n"),
                                cvar->resetValue.c_str(), it.first.c_str(), result.description.c_str());
                    }
                }
            }
        }
    }

    // Used by the C API

    cvar_t* FindCCvar(const std::string& cvarName) {
        CvarMap& cvars = GetCvarMap();

        auto iter = cvars.find(cvarName);
        if (iter != cvars.end()) {
            return &iter->second->ccvar;
        }

        return nullptr;
    }

    void WriteVariables(fileHandle_t f) {
        CvarMap& cvars = GetCvarMap();

        for (auto it : cvars) {
            cvarRecord_t* cvar = it.second;

            if (cvar->flags & ARCHIVE) {
                FS_Printf(f, "seta %s %s\n", it.first.c_str(), Cmd::Escape(cvar->value).c_str());
            }
        }
    }

    char* InfoString(int flag, bool big) {
        static char info[BIG_INFO_STRING];
        info[0] = 0;

        CvarMap& cvars = GetCvarMap();

        for (auto it : cvars) {
            cvarRecord_t* cvar = it.second;

            if (cvar->flags & flag) {
                Info_SetValueForKey(info, it.first.c_str(), cvar->value.c_str(), big);
            }
        }

        return info;
    }

    void SetValueCProxy(const std::string& cvarName, std::string value) {
        InternalSetValue(cvarName, std::move(value), 0, true, false);
    }

    /*
    ===============================================================================

    Cvar:: Cvar-related commands

    ===============================================================================
    */

    class SetCmd: public Cmd::StaticCmd {
        public:
            SetCmd(const std::string& name, int flags): Cmd::StaticCmd(name, Cmd::BASE, N_("sets the value of a cvar")), flags(flags) {
            }

            void Run(const Cmd::Args& args) const OVERRIDE {
                int argc = args.Argc();
                int nameIndex = 1;
                bool unsafe = false;

                if (argc < 3) {
                    PrintUsage(args, _("[-unsafe] <variable> <value>"), "");
                    return;
                }

                if (argc >= 4 and args.Argv(1) == "-unsafe") {
                    nameIndex = 2;
                    unsafe = true;
                }

                const std::string& name = args.Argv(nameIndex);

                //TODO
                if (unsafe and com_crashed != nullptr and com_crashed->integer != 0) {
                    Print(_("%s is unsafe. Check com_crashed."), name.c_str());
                    return;
                }

                const std::string& value = args.Argv(nameIndex + 1);

                ::Cvar::SetValue(name, value);
                ::Cvar::AddFlags(name, flags);
            }

            Cmd::CompletionResult Complete(int argNum, const Cmd::Args& args, Str::StringRef prefix) const OVERRIDE {
                if (argNum == 1 or (argNum == 2 and args.Argv(1) == "-unsafe")) {
                    return ::Cvar::Complete(prefix);
                }

                return {};
            }

        private:
            int flags;
    };
    static SetCmd SetCmdRegistration("set", 0);
    static SetCmd SetuCmdRegistration("setu", USERINFO);
    static SetCmd SetsCmdRegistration("sets", SERVERINFO);
    static SetCmd SetaCmdRegistration("seta", ARCHIVE);

    class ResetCmd: public Cmd::StaticCmd {
        public:
            ResetCmd(): Cmd::StaticCmd("reset", Cmd::BASE, N_("resets a variable")) {
            }

            void Run(const Cmd::Args& args) const OVERRIDE {
                if (args.Argc() != 2) {
                    PrintUsage(args, _("<variable>"), "");
                    return;
                }

                const std::string& name = args.Argv(1);
                CvarMap& cvars = GetCvarMap();

                auto iter = cvars.find(name);
                if (iter != cvars.end()) {
                    cvarRecord_t* cvar = iter->second;
                    ::Cvar::SetValue(name, cvar->resetValue);
                } else {
                    Print(_("Cvar '%s' doesn't exist"), name.c_str());
                }
            }

            Cmd::CompletionResult Complete(int argNum, const Cmd::Args& args, Str::StringRef prefix) const OVERRIDE {
                Q_UNUSED(args);

                if (argNum == 1) {
                    return ::Cvar::Complete(prefix);
                }

                return {};
            }
    };
    static ResetCmd ResetCmdRegistration;

    std::string Raw(const std::string& src) {
        std::string out;
        size_t length = src.length();

        for (size_t i = 0; i < length; ++i)
        {
            if (src[i] == '^')
                out += '^';
            out += src[i];
        }
        return out;
    }

    class ListCvars: public Cmd::StaticCmd {
        public:
            ListCvars(): Cmd::StaticCmd("listCvars", Cmd::BASE, N_("lists variables")) {
            }

            void Run(const Cmd::Args& args) const OVERRIDE {
                CvarMap& cvars = GetCvarMap();

                bool raw = false;
                std::string match = "";

                //Read parameters
                if (args.Argc() > 1) {
                    match = args.Argv(1);
                    if (match == "-raw") {
                        raw = true;
                        match = (args.Argc() > 2) ? args.Argv(2) : "";
                    }
                }

                std::vector<cvarRecord_t*> matches;

                std::vector<std::string> matchesNames;
                unsigned long maxNameLength = 0;

                std::vector<std::string> matchesValues;
                unsigned long maxValueLength = 0;

                //Find all the matching cvars
                for (auto& record : cvars) {
                    if (Q_stristr(record.first.c_str(), match.c_str())) {
                        matchesNames.push_back(record.first);

                        matches.push_back(record.second);
                        matchesValues.push_back(record.second->value);

                        //TODO: the raw parameter is not handled, need a function to escape carets
                        maxNameLength = std::max<size_t>(maxNameLength, record.first.length());
                        maxValueLength = std::max<size_t>(maxValueLength, matchesValues.back().length());
                    }
                }

                //Do not pad descriptions too much
                maxValueLength = std::min(maxValueLength, 20UL);

                //Print the matches, keeping the flags and descriptions aligned
                for (unsigned i = 0; i < matches.size(); i++) {
                    const std::string& name = matchesNames[i];
                    const std::string& value = matchesValues[i];
                    cvarRecord_t* var = matches[i];

                    std::string filler1 = std::string(maxNameLength - name.length(), ' ');

                    std::string flags = "";
                    flags += (var->flags & SERVERINFO) ? "S" : "_";
                    flags += (var->flags & SYSTEMINFO) ? "s" : "_";
                    flags += (var->flags & USERINFO) ? "U" : "_";
                    flags += (var->flags & ROM) ? "R" : "_";
                    flags += (var->flags & CVAR_INIT) ? "I" : "_";
                    flags += (var->flags & ARCHIVE) ? "A" : "_";
                    flags += (var->flags & CVAR_LATCH) ? "L" : "_";
                    flags += (var->flags & CHEAT) ? "C" : "_";
                    flags += (var->flags & CVAR_USER_CREATED) ? "?" : "_";

                    int padding = maxValueLength - value.length();
                    std::string filler2;
                    if (padding > 0) {
                        filler2 = std::string(maxValueLength - value.length(), ' ');
                    }

                    // this is going to 'break' (wrong output) if the description contains any ^s other than in the variable value(s)
                    if (raw) {
                        Print("  %s%s %s %s", name, filler1, flags, Raw(var->description));
                    }
                    else {
                        Print("  %s%s %s %s", name, filler1, flags, var->description);
                    }
                }

                Print("%zu cvars", matches.size());
            }

            Cmd::CompletionResult Complete(int argNum, const Cmd::Args& args, Str::StringRef prefix) const OVERRIDE {
                Q_UNUSED(args);

                //TODO handle -raw?
                if (argNum == 1) {
                    return ::Cvar::Complete(prefix);
                }

                return {};
            }
    };
    static ListCvars ListCvarsRegistration;

}
