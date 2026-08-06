#include "fpp-pch.h"

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <cinttypes>

#include "FPPArtNetAdv.h"
#include "Warnings.h"
#include "e131bridge.h"
#include "commands/Commands.h"

#include "Plugin.h"
#include "MultiSync.h"
#include "playlist/Playlist.h"
#include "channeloutput/channeloutputthread.h"


class FPPArtNetAdvPlugin : public FPPPlugins::Plugin, public FPPPlugins::PlaylistEventPlugin, public FPPPlugins::APIProviderPlugin, public MultiSyncPlugin {
    static constexpr int TIMECODE_PACKET_LEN = 19;
    static constexpr uint8_t TIMECODE_PACKET_HEADER[TIMECODE_PACKET_LEN] = {
        'A', 'r', 't', '-', 'N', 'e', 't', 0x00, // 8-byte ID
        0x00,                                    // Opcode Low
        0x97,                                    // Opcode High
        0x00,                                    // Protocol Version High
        0x0E,                                    // Protocol Version Low
        0x00,                                    // Aux1
        0x00                                     // Aux2
    };
    static constexpr int ARTNET_PORT = 6454;
    
public:
    // The "true" asks FPP to watch config/plugin.fpp-ArtNetAdv and call
    // settingChanged() below, so retuning timecode or triggers no longer
    // needs an fppd restart.
    FPPArtNetAdvPlugin() : FPPPlugins::Plugin("fpp-ArtNetAdv", true), FPPPlugins::PlaylistEventPlugin(), FPPPlugins::APIProviderPlugin() {
        LogInfo(VB_PLUGIN, "Initializing ArtNetAdv Plugin\n");
        setDefaultSettings();
    }
    virtual ~FPPArtNetAdvPlugin() {
        // Everything here is also done in shutdown(); both are idempotent, and
        // keeping them means a teardown path that does not go through
        // shutdown() still leaves nothing pointing at this object.
        RemoveArtNetOpcodeHandler(0x9700);
        RemoveArtNetOpcodeHandler(0x9900);
        MultiSync::INSTANCE.removeMultiSyncPlugin(this);
    }

    // Withdraw everything holding a pointer into this plugin. A destructor is
    // too late for the MultiSync registration in particular: SendSeqSyncPacket()
    // is called from the sequence/media path, so leaving it registered while the
    // object is being destroyed is a call into a half-destroyed plugin.
    virtual std::function<bool()> shutdown() override {
        // Lambdas capturing 'this', held in FPP's ArtNet opcode table.
        RemoveArtNetOpcodeHandler(0x9700);
        RemoveArtNetOpcodeHandler(0x9900);
        MultiSync::INSTANCE.removeMultiSyncPlugin(this);
        // ArtNetTriggerCommand is declared in this plugin, so its vtable lives
        // in this .so. removeCommand() only unregisters - CommandManager owns
        // what stays in its registry - so taking it back means deleting it.
        for (Command* c : myCommands) {
            CommandManager::INSTANCE.removeCommand(c);
            delete c;
        }
        myCommands.clear();
        // This plugin raised the warning, so it takes it back rather than
        // leaving a stale one on the UI for a plugin that is no longer loaded.
        WarningHolder::RemoveWarning(MULTISYNC_WARNING);
        return nullptr;
    }
    
    static in_addr_t toInetAddr(const std::string& ipAddress, bool& valid) {
        valid = true;
        bool isAlpha = false;
        for (int x = 0; x < ipAddress.length(); x++) {
            isAlpha |= isalpha(ipAddress[x]);
        }

        if (isAlpha) {
            struct hostent* uhost = gethostbyname(ipAddress.c_str());
            if (!uhost) {
                valid = false;
                return 0;
            } else {
                return *((unsigned long*)uhost->h_addr);
            }
        }
        return inet_addr(ipAddress.c_str());
    }
    class ArtNetTriggerCommand : public Command {
    public:
        ArtNetTriggerCommand() : Command("Send ArtNet Trigger") {
            args.push_back(CommandArg("TargetIP", "string", "Target IP").setDefaultValue("255.255.255.255"));
            args.push_back(CommandArg("OEMCode", "string", "OEM Code (hex)").setDefaultValue("0xFFFF"));
            args.push_back(CommandArg("Key", "int", "Key").setRange(0, 255));
            args.push_back(CommandArg("SubKey", "int", "SubKey").setRange(0, 255));
            args.push_back(CommandArg("Payload", "string", "Payload"));
        }
        virtual std::unique_ptr<Command::Result> run(const std::vector<std::string>& args) override {
            uint8_t packet[700] = {
                'A', 'r', 't', '-', 'N', 'e', 't', 0x00, // 8-byte ID
                0x00,                                    // Opcode Low
                0x99,                                    // Opcode High
                0x00,                                    // Protocol Version High
                0x0E,                                    // Protocol Version Low
                0x00,                                    // Aux1
                0x00                                     // Aux2
            };

            bool valid = true;
            std::string ip = args[0];
            std::string oem = args[1];
            int oemInt = std::stoul(oem, nullptr, 16);
            std::string key = args[2];
            std::string subkey = args[3];

            packet[14] = oemInt >> 8;
            packet[15] = oemInt & 0xFF;
            packet[16] = std::stol(key);
            packet[17] = std::stol(subkey);
            strcpy((char *)&packet[18], args[4].c_str());

            int socket = CreateArtNetSocket();
            struct sockaddr_in dest_addr;
            memset(&dest_addr, 0, sizeof(dest_addr));
            in_addr_t addr = toInetAddr(ip, valid);
            dest_addr.sin_addr.s_addr = addr;
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(ARTNET_PORT);
            sendto(socket, packet, 19 + args[4].length(), 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
            return std::make_unique<Command::Result>("ArtTrigger Sent");
        }
    };

    bool Bridge_ProcessArtNetTrigger(uint8_t* bridgeBuffer, long long packetTime) {
        int oem = (bridgeBuffer[14] << 8) | bridgeBuffer[15];
        int key = bridgeBuffer[16];
        if (oem == triggerOem && key == 0) {
            std::string payload = (const char *)&bridgeBuffer[18];
            Json::Value val = LoadJsonFromString(payload);
            if (val.isObject()) {
                CommandManager::INSTANCE.run(val);
            } else {
                LogWarn(VB_COMMAND, "Invalid JSON Payload for ArtTrigger: %s\n", payload.c_str());
            }
        }
        return false;
    }
    bool Bridge_ProcessArtNetTimeCode(uint8_t* bridgeBuffer, long long packetTime) {
        if (bridgeBuffer[9] == 0x97 && bridgeBuffer[8] == 0x00) {
            //ArtNet timecode
            int frames = bridgeBuffer[14];
            int seconds = bridgeBuffer[15];
            int minutes = bridgeBuffer[16];
            int hours = bridgeBuffer[17];
            int type = bridgeBuffer[18];
            uint64_t ms = 0;
            switch (type) {
            case 1: //ebu (25fps)
                ms = frames * 40;
                break;
            case 2: //df (29.97fps)
                ms = ((float)frames * 33.3667f);
                break;
            case 3: //smpte(30 fps)
                ms = ((float)frames * 33.3333f);
                break;
            case 0: //film 24fps
            default:
                ms = ((float)frames * 41.6667f);
                break;
            }
            uint64_t oms = ms;
            ms += seconds * 1000;
            ms += minutes * 60 * 1000;
            ms += hours * 60 * 60 * 1000;

            int idx = 0;
            std::string pl = "";
            std::string f = settings["ArtNetSyncPlaylist"];
            if (FileExists(FPP_DIR_PLAYLIST("/" + f + ".json"))) {
                pl = f;
            }
            if (pl == "--none--") {
                pl = "";
            }

            if (timeCodePType == TimeCodeProcessingType::HOUR) {
                constexpr int DIV = 1000 * 60 * 60;
                idx = ms / DIV;
                ms %= DIV;
            } else if (timeCodePType == TimeCodeProcessingType::MIN15) {
                constexpr int DIV = 1000 * 60 * 15;
                idx = ms / DIV;
                ms %= DIV;
            } else if (timeCodePType == TimeCodeProcessingType::PLAYLIST_ITEM_DEFINED) {
                idx = -2;
            } else {
                idx = -1;
            }
            LogDebug(VB_E131BRIDGE, "ArtNet Timestamp:  %d     Playlist: %s\n", ms, pl.c_str());
            //printf("ArtNet Timestamp:  %" PRIu64 "     Playlist: %s     %d:%d:%d.%d\n", ms, pl.c_str(), hours, minutes, seconds, (int)oms);
            if (pl != "") {
                if (oms == 0 && hours == 0 && minutes == 0 && seconds == 0) {
                    //stop command
                    MultiSync::INSTANCE.SyncStopAll();
                } else {
                    MultiSync::INSTANCE.SyncPlaylistToMS(ms, idx, pl, false);
                }
            }
        }
        return false;
    }


    uint64_t getTimestampFromPlaylist() {
        int pos;
        uint64_t ms;
        uint64_t posms;
        
        ms = playlist->GetCurrentPosInMS(pos, posms, timeCodePType == TimeCodeProcessingType::PLAYLIST_ITEM_DEFINED);
        if (timeCodePType == TimeCodeProcessingType::HOUR) {
            ms = posms + pos * 60 * 1000 * 60;
        } else if (timeCodePType == TimeCodeProcessingType::MIN15) {
            ms = posms + pos * 15 * 1000 * 60;
        }
        return ms == 0 ? 1 : ms;  // zero is stop so we will use 1ms as a starting point
    }

    virtual void SendSeqSyncPacket(const std::string &filename, int frames, float seconds) override {
        SendTimeCode(getTimestampFromPlaylist());
    }
    virtual void SendMediaSyncPacket(const std::string &filename, float seconds) override {
        SendTimeCode(getTimestampFromPlaylist());
    }
    

    virtual void playlistCallback(const Json::Value &plj, const std::string &action, const std::string &section, int item) override {
        if (action == "stop") {
            SendTimeCode(0);
        } else if (action == "start") {
        } else if (action == "playing") {
        }
    }
    void SendTimeCode(uint64_t newTc) {
        if (newTc != lastTimecode) {
            lastTimecode = newTc; 

            int frames = newTc % 1000;
            int seconds = (newTc / 1000) % 60;
            int minutes = (newTc / 60000) % 60;
            int hours = newTc / (60000 * 60);

            switch (timecodeType) {
            case 1: //ebu (25fps)
                frames = frames / 40;
                break;
            case 2: //df (29.97fps)
                frames = ((float)frames / 33.3667f);
                break;
            case 3: //smpte(30 fps)
                frames = ((float)frames / 33.3333f);
                break;
            case 0: //film 24fps
            default:
                frames = ((float)frames / 41.6667f);
                break;
            }
            if (newTc == 1) {
                // all zeros would represent a stop so we need to go to frame 1
                frames = 1;
            }
            for (auto &b : destBuffers) {
                b[14] = frames;
                b[15] = seconds;
                b[16] = minutes;
                b[17] = hours;
            }
            sendmmsg(artnetSocket, &destMessages[0], destMessages.size(), MSG_DONTWAIT);
        }
    }
    void InitializeTimeCodeSend() {
        std::vector<std::string> ips = split(settings["ArtNetTimeCodeTarget"], ',');

        if (!ips.empty()) {
            // User input, and this now runs whenever a setting changes rather
            // than only at startup, so a throw here would land in FPP's
            // file-monitor callback on the main loop and take fppd down.
            timecodeType = safeStoi(settings["ArtNetTimeCodeType"], 3);

            // Cleared rather than only resized: this is re-run on a settings
            // change, and a shorter target list would otherwise leave stale
            // destinations behind to keep receiving timecode.
            destMessages.clear();
            destIOVs.clear();
            destBuffers.clear();
            destAddresses.clear();
            destMessages.resize(ips.size());
            destIOVs.resize(ips.size());
            destBuffers.resize(ips.size());
            destAddresses.resize(ips.size());
            for (int x = 0; x < ips.size(); x++) {
                bool valid = false;
                destAddresses[x].sin_addr.s_addr = toInetAddr(ips[x], valid);
                destAddresses[x].sin_family = AF_INET;
                destAddresses[x].sin_port = htons(ARTNET_PORT);
                destMessages[x].msg_hdr.msg_name = &destAddresses[x];
                destMessages[x].msg_hdr.msg_namelen = sizeof(struct sockaddr_in);
                destMessages[x].msg_hdr.msg_iov = &destIOVs[x];
                destMessages[x].msg_hdr.msg_iovlen = 1;
                destIOVs[x].iov_base = &destBuffers[x];
                destIOVs[x].iov_len = TIMECODE_PACKET_LEN;
                memcpy(&destBuffers[x][0], TIMECODE_PACKET_HEADER, TIMECODE_PACKET_LEN);
                destBuffers[x][18] = timecodeType;
            }
            MultiSync::INSTANCE.addMultiSyncPlugin(this);
            if (!MultiSync::INSTANCE.isMultiSyncEnabled()) {
                WarningHolder::AddWarning(MULTISYNC_WARNING);
            }
            artnetSocket = CreateArtNetSocket();
        }
    }

    virtual void addControlCallbacks(std::map<int, std::function<bool(int)>> &callbacks) override {
        Command* trigger = new ArtNetTriggerCommand();
        myCommands.push_back(trigger);
        CommandManager::INSTANCE.addCommand(trigger);

        // Nothing to hand FPP here: the ArtNet receive descriptor is shared
        // with e131bridge, which owns its epoll registration and puts it in the
        // set for as long as any opcode handler wants packets. Registering it
        // from here would replace whatever callback the bridge had, and
        // withdrawing it on unload would stop the bridge receiving ArtNet.
        applyConfiguration();
    }

    // Called by FPP when config/plugin.fpp-ArtNetAdv changes; the base class has
    // already updated settings[key]. Main loop, so reinstalling the handlers
    // inline is safe.
    virtual void settingChanged(const std::string &key, const std::string &value) override {
        LogInfo(VB_PLUGIN, "ArtNetAdv: %s changed, reconfiguring\n", key.c_str());
        // Adding and removing the opcode handlers is enough - e131bridge puts
        // the shared ArtNet socket in and out of the epoll set to match.
        applyConfiguration();
    }

    // Reinstall the opcode handlers and the timecode sender from the current
    // settings. Idempotent - it withdraws whatever is in place first - which is
    // what lets a settings change take effect without restarting fppd.
    void applyConfiguration() {
        // Withdraw first: these are lambdas capturing this, held in FPP's opcode
        // table, and InitializeTimeCodeSend() re-adds the MultiSync plugin.
        RemoveArtNetOpcodeHandler(0x9700);
        RemoveArtNetOpcodeHandler(0x9900);
        MultiSync::INSTANCE.removeMultiSyncPlugin(this);
        WarningHolder::RemoveWarning(MULTISYNC_WARNING);
        lastTimecode = 0;

        std::string tcpt = settings["ArtNetTimeCodeProcessing"];
        if (tcpt == "1") {
            timeCodePType = TimeCodeProcessingType::HOUR;
        } else if (tcpt == "2") {
            timeCodePType = TimeCodeProcessingType::MIN15;
        } else if (tcpt == "3") {
            timeCodePType = TimeCodeProcessingType::PLAYLIST_ITEM_DEFINED;
        } else {
            timeCodePType = TimeCodeProcessingType::PLAYLIST_POS;
        }

        if (settings["ArtNetTimeCodeEnabled"] == "1") {
            if (getFPPmode() != REMOTE_MODE) {
                InitializeTimeCodeSend();
            } else {
                std::function<bool(uint8_t*, long long)> f = [this](uint8_t* bridgeBuffer, long long packetTime) {
                    return Bridge_ProcessArtNetTimeCode(bridgeBuffer, packetTime);
                };
                AddArtNetOpcodeHandler(0x9700, f);
            }
        }
        if (settings["ArtNetTriggerEnabled"] == "1") {
            std::function<bool(uint8_t*, long long)> f = [this](uint8_t* bridgeBuffer, long long packetTime) {
                return Bridge_ProcessArtNetTrigger(bridgeBuffer, packetTime);
            };
            AddArtNetOpcodeHandler(0x9900, f);
            // User input, and this now runs whenever the setting changes rather
            // than only at startup - std::stoul() throwing inside FPP's
            // file-monitor callback would take fppd down.
            triggerOem = safeStoulHex(settings["ArtNetTriggerOEMCode"], 0x2100);
        }
    }
    static int safeStoi(const std::string &s, int defVal) {
        try {
            if (!s.empty()) {
                return std::stoi(s);
            }
        } catch (const std::exception &e) {
            LogErr(VB_PLUGIN, "ArtNetAdv: bad numeric setting \"%s\": %s - using %d\n", s.c_str(), e.what(), defVal);
        }
        return defVal;
    }
    static int safeStoulHex(const std::string &s, int defVal) {
        try {
            if (!s.empty()) {
                return (int)std::stoul(s, nullptr, 16);
            }
        } catch (const std::exception &e) {
            LogErr(VB_PLUGIN, "ArtNetAdv: bad OEM code \"%s\": %s - using 0x%X\n", s.c_str(), e.what(), defVal);
        }
        return defVal;
    }
    
    void setDefaultSettings() {
        setIfNotFound("ArtNetTimeCodeEnabled", "0");
        setIfNotFound("ArtNetSyncPlaylist", "");
        setIfNotFound("ArtNetTriggerEnabled", "1");
        setIfNotFound("ArtNetTimeCodeTarget", "255.255.255.255");
        setIfNotFound("ArtNetTimeCodeProcessing", "0");
        setIfNotFound("ArtNetTimeCodeType", "3");
        setIfNotFound("ArtNetTriggerOEMCode", "0x2100");
    }
    void setIfNotFound(const std::string &s, const std::string &v, bool emptyAllowed = false) {
        if (settings.find(s) == settings.end()) {
            settings[s] = v;
        } else if (!emptyAllowed && settings[s] == "") {
            settings[s] = v;
        }
    }
    

    enum class TimeCodeProcessingType {
        PLAYLIST_POS,
        HOUR,
        MIN15,
        PLAYLIST_ITEM_DEFINED
    } timeCodePType;

    int triggerOem = 0x2100;

    int timecodeType = 3;
    uint64_t lastTimecode = 0;
    int artnetSocket = -1;
    std::vector<Command*> myCommands;
    static constexpr const char* MULTISYNC_WARNING =
        "ArtNet TimeCode Sync enabled, but MultiSync is not enabled.  No TimeCodes will be sent.";
    std::vector<struct mmsghdr> destMessages;
    std::vector<struct sockaddr_in> destAddresses;
    std::vector<struct iovec> destIOVs;
    std::vector<std::array<uint8_t, TIMECODE_PACKET_LEN>> destBuffers;
};


// Safe to dlclose() on unload: no threads, no timers, no CurlManager requests
// and no HTTP routes. The ArtNet receive socket is registered through
// addControlCallbacks(), so FPP withdraws it from the epoll loop before
// shutdown() runs. shutdown() takes back the opcode handlers, the MultiSync
// registration and the trigger command - the things that hold a pointer into
// this library.
FPP_PLUGIN_SUPPORTS_UNLOAD()

extern "C" {
    FPPPlugins::Plugin *createPlugin() {
        return new FPPArtNetAdvPlugin();
    }
}
