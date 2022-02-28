#include "fpp-pch.h"

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>

#include "FPPArtNetAdv.h"
#include "Warnings.h"
#include "e131bridge.h"
#include "commands/Commands.h"

#include "Plugin.h"
#include "MultiSync.h"
#include "playlist/Playlist.h"
#include "channeloutput/channeloutputthread.h"


class FPPArtNetAdvPlugin : public FPPPlugin, public MultiSyncPlugin {
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
    FPPArtNetAdvPlugin() : FPPPlugin("fpp-ArtNetAdv") {
        LogInfo(VB_PLUGIN, "Initializing ArtNetAdv Plugin\n");
        setDefaultSettings();
    }
    virtual ~FPPArtNetAdvPlugin() {
        MultiSync::INSTANCE.removeMultiSyncPlugin(this);
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
            int ms = 0;
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

            ms += seconds * 1000;
            ms += minutes * 60 * 1000;
            if (!timecodeHourIsIndex) {
                ms += hours * 60 * 60 * 1000;
            }
            std::string pl = "";
            std::string f = settings["ArtNetSyncPlaylist"];
            if (FileExists(FPP_DIR_PLAYLIST("/" + f + ".json"))) {
                pl = f;
            }
            if (pl == "--none--") {
                pl = "";
            }
            LogDebug(VB_E131BRIDGE, "ArtNet Timestamp:  %d     Playlist: %s\n", ms, pl.c_str());

            if (pl != "") {
                if (ms == 0 && hours == 0) {
                    //stop command
                    MultiSync::INSTANCE.SyncStopAll();
                } else if (timecodeHourIsIndex) {
                    MultiSync::INSTANCE.SyncPlaylistToMS(ms, hours, pl, false);
                } else {
                    MultiSync::INSTANCE.SyncPlaylistToMS(ms, pl, false);
                }
            }
        }
        return false;
    }


    uint64_t getTimestampFromPlaylist() {
        int pos;
        uint64_t ms;
        uint64_t posms;
        
        ms = playlist->GetCurrentPosInMS(pos, posms);
        if (timecodeHourIsIndex) {
            ms = posms + pos * 60000 * 60;
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
            timecodeType = std::stoi(settings["ArtNetTimeCodeType"]);

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
                WarningHolder::AddWarning("ArtNet TimeCode Sync enabled, but MultiSync is not enabled.  No TimeCodes will be sent.");
            }
            artnetSocket = CreateArtNetSocket();
        }
    }

    virtual void addControlCallbacks(std::map<int, std::function<bool(int)>> &callbacks) override {
        CommandManager::INSTANCE.addCommand(new ArtNetTriggerCommand());

        bool handlerAdded = false;
        timecodeHourIsIndex = settings["ArtNetTimeCodeHourIsIndex"] == "1";

        if (settings["ArtNetTimeCodeEnabled"] == "1") {
            if (getFPPmode() != REMOTE_MODE) {
                InitializeTimeCodeSend();
            } else {
                std::function<bool(uint8_t*, long long)> f = [this](uint8_t* bridgeBuffer, long long packetTime) {
                    return Bridge_ProcessArtNetTimeCode(bridgeBuffer, packetTime);
                };
                AddArtNetOpcodeHandler(0x9700, f);
                handlerAdded = true;
            }
        }
        if (settings["ArtNetTriggerEnabled"] == "1") {
            std::function<bool(uint8_t*, long long)> f = [this](uint8_t* bridgeBuffer, long long packetTime) {
                return Bridge_ProcessArtNetTrigger(bridgeBuffer, packetTime);
            };
            AddArtNetOpcodeHandler(0x9900, f);
            triggerOem = std::stoul(settings["ArtNetTriggerOEMCode"], nullptr, 16);
            handlerAdded = true;
        }
        if (handlerAdded) {
            std::function<bool(int)> f = [](int i) {
                return Bridge_ReceiveArtNetData();
            };
            callbacks[CreateArtNetSocket()] = f;
        }
    }
    
    void setDefaultSettings() {
        setIfNotFound("ArtNetTimeCodeEnabled", "0");
        setIfNotFound("ArtNetSyncPlaylist", "");
        setIfNotFound("ArtNetTriggerEnabled", "1");
        setIfNotFound("ArtNetTimeCodeTarget", "255.255.255.255");
        setIfNotFound("ArtNetTimeCodeHourIsIndex", "0");
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
    

    int triggerOem = 0x2100;

    int timecodeType = 3;
    bool timecodeHourIsIndex = false;
    uint64_t lastTimecode = 0;
    int artnetSocket = -1;
    std::vector<struct mmsghdr> destMessages;
    std::vector<struct sockaddr_in> destAddresses;
    std::vector<struct iovec> destIOVs;
    std::vector<std::array<uint8_t, TIMECODE_PACKET_LEN>> destBuffers;
};


extern "C" {
    FPPPlugin *createPlugin() {
        return new FPPArtNetAdvPlugin();
    }
}
