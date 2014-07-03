/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "WardenMgr.h"
#include "World.h"
#include "Policies/SingletonImp.h"
#include "ProgressBar.h"

/*
Useful information:

- A module is chosen for 24H and saved. Next client connection the same day will reuse the
  same module.
- The client is kicked is he failed to load a module after the module is sent.
- We ask the client to run between 5 and 9 cheat checks and we sent this request 12 to 15
  seconds after his last reply.
- The client is kicked if it did not reply 2 minutes after the cheat checks packet is sent
- The client is kicked/banned if one test failed.
*/

WardenMgr::WardenMgr() : m_Disconnected(false), m_Banning(false), m_HalfCall(false)
{
    #if defined (ACE_HAS_EVENT_POLL) || defined (ACE_HAS_DEV_POLL)
        ACE_Reactor::instance(new ACE_Reactor(new ACE_Dev_Poll_Reactor(ACE::max_handles(), 1), 1), true);
    #endif
}

WardenMgr::~WardenMgr()
{
}

void WardenMgr::Initialize(const char *addr, u_short port, bool IsBanning)
{
    // Save the address and port
    m_WardendAddress = addr;
    m_WardendPort = port;
    m_Banning = IsBanning;

    if (!LoadFromDB())
    {
        sLog.outError("Warden disabled because there is no usable module or 1 table has no record");
        m_Enabled = false;
        return;
    }

    if (m_Disconnected = !InitializeCommunication())
    {
        sLog.outError("Warden Daemon not reachable, trying to connect in the background");
        m_PingOut = true;
    }

    m_PingTimer.SetInterval(10 * IN_MILLISECONDS);          // 10 secs
    m_PingTimer.Reset();
}

bool WardenMgr::InitializeCommunication()
{
    // Establish the connection
    m_Enabled = true;
    WardenSvcHandler* handler = new WardenSvcHandler;

    ACE_INET_Addr remoteAddr(m_WardendPort, m_WardendAddress.c_str());
    if (m_Connector.connect(handler, remoteAddr) == -1)
    {
        return false;
    }

    m_WardenProcessStream = handler->Peer;
    ByteBuffer pkt;
    const char *sign = WARDEND_SIGN;
    pkt << sign;
    m_WardenProcessStream->send((char const*)pkt.contents(), pkt.size());

    m_PingOut = false;
    return true;
}

// Triggered by world every 500ms
void WardenMgr::Update(uint32 diff)
{
    if (!m_Enabled)
        return;

    m_PingTimer.Update(diff);

    if (m_PingTimer.Passed())
    {
        if (m_PingOut && m_Disconnected)
        {
            m_Disconnected = !InitializeCommunication();
            m_PingTimer.Reset();
        }
        else if (m_PingOut && !m_Disconnected)
        {
            SetDisconnected();
        }
        else
        {
            SendPing();
            m_PingTimer.Reset();
        }
    }

    ACE_Time_Value t(0.001);
    int res = ACE_Reactor::instance()->run_reactor_event_loop(t);
}

// Triggered by a session
void WardenMgr::Update(WorldSession* const session)
{
    m_HalfCall = !m_HalfCall;                           // To return half of the time since called 2 times
    if (!m_HalfCall)
        return;

    if (session->m_WardenTimer.Passed())                // We don't care the connection to wardend state to do cheat-checks or register
    {
        switch (session->m_wardenStatus)
        {
            case WARD_STATE_UNREGISTERED:               // register a client that could not register earlier
                StartForSession(session);
                return;
            case WARD_STATE_LOAD_MODULE:                // no reply to load module request (20 secs)
            case WARD_STATE_LOAD_FAILED:                // no reply after we sent the module to client (20 secs)
                BASIC_LOG("Warden Manager: no reply received for module load or 2 times load failed, kicking account %u", session->GetAccountId());
                session->KickPlayer();
                return;
            case WARD_STATE_TRANSFORM_SEED:             // no reply to transformed seed (20 secs)
                BASIC_LOG("Warden Manager: no transformed seed received, kicking account %u", session->GetAccountId());
                session->KickPlayer();
                return;
            case WARD_STATE_CHEAT_CHECK_OUT:            // timeout waiting for a cheat check reply
                BASIC_LOG("Warden Manager: no Cheat-check reply received, kicking account %u", session->GetAccountId());
                session->KickPlayer();
                return;
            case WARD_STATE_CHEAT_CHECK_IN:             // send cheat check
                SendCheatCheck(session);
                session->m_wardenStatus = WARD_STATE_CHEAT_CHECK_OUT;
                session->m_WardenTimer.SetInterval(2 * MINUTE * IN_MILLISECONDS);
                session->m_WardenTimer.SetCurrent(0);   // 2 full minutes
                return;
            default:
                break;
        }
    }

    if (m_Disconnected)
    {
        session->m_WardenTimer.SetInterval(15 * IN_MILLISECONDS); // push back warden activity in session by 15 seconds
        session->m_WardenTimer.Reset();
        if (session->m_wardenStatus == WARD_STATE_PENDING_WARDEND)
            session->m_wardenStatus = WARD_STATE_NEED_WARDEND;     // We needed data, so have to redo the request
        return;
    }

    // Connected to wardend, last time was disconnected, then resume and re-ask for module load and key generation
    if (session->m_WardenTimer.Passed() && session->m_wardenStatus == WARD_STATE_NEED_WARDEND)
    {
        LoadModuleAndGetKeys(session);
        session->m_WardenTimer.SetInterval(10 * IN_MILLISECONDS);
        session->m_WardenTimer.Reset();
        session->m_wardenStatus = WARD_STATE_PENDING_WARDEND;
    }
}

void WardenMgr::SetDisconnected()
{
    if (!m_Disconnected)
    {
        sLog.outError("Connection to Warden Daemon lost, trying to reconnect in the background");
        m_Connector.close();
        m_PingTimer.SetCurrent(m_PingTimer.GetInterval()); // expire it
        m_Disconnected = true;
    }
}

bool WardenMgr::LoadFromDB()
{
    QueryResult *result = WorldDatabase.Query("SELECT md5,chk0,chk1,chk2,chk3,chk4,chk5,chk6,chk7,chk8,end9 FROM warden_module");
    if (!result)
    {
        m_WardenModuleMap.clear();
        sLog.outString(">> Table warden_module is empty!");
        sLog.outString();
        return false;
    }

    uint32 count = 0;
    {
        barGoLink bar((int)result->GetRowCount());
        do
        {
            Field *fields = result->Fetch();
            bar.step();

            std::string md5 = fields[0].GetCppString();
            if (CheckModuleExistOnDisk(md5))
            {
                WardenCheckMap& moduleCheck = m_WardenModuleMap[md5];
                moduleCheck.resize(10);

                for (uint8 i=0; i<=9; ++i)
                {
                    moduleCheck[i] = fields[i+1].GetUInt8();
                }

                ++count;
            }
            else
                sLog.outError("Module %s has a record in 'warden_module' but no binary on disk, skiping it", md5.c_str());

        } while(result->NextRow());
        delete result;

        sLog.outString();
        sLog.outString(">> Loaded %u warden modules", count);
        if (count == 0)
            return false;

    }
    // Now load the checks
    // 1) memory
    result = WorldDatabase.Query("SELECT String,Offset,Length,Result,Comment FROM warden_check_memory");
    if (!result)
    {
        sLog.outString(">> Table warden_check_memory is empty!");
        sLog.outString();
    }
    else
    {
        uint32 count = 0;
        {
            barGoLink bar((int)result->GetRowCount());
            m_WardenMemoryChecks.resize((int)result->GetRowCount());
            do
            {
                Field *fields = result->Fetch();
                bar.step();

                MemoryCheckEntry& current = m_WardenMemoryChecks[count];
                current.String  = fields[0].GetCppString();
                current.Offset  = fields[1].GetUInt32();
                current.Length  = fields[2].GetUInt8();
                std::string res = fields[3].GetCppString();
                current.Comment = fields[4].GetCppString();
                hexDecodeString(res.c_str(), res.length(), current.Result);

                ++count;
            } while(result->NextRow());
            delete result;
            sLog.outString();
            sLog.outString(">> Loaded %u memory checks", count);
        }
    }
    // 2) Page_A
    result = WorldDatabase.Query("SELECT Seed,SHA,Offset,Length FROM warden_check_page_a");
    if (!result)
    {
        sLog.outString(">> Table warden_check_page_a is empty!");
        sLog.outString();
    }
    else
    {
        uint32 count = 0;
        {
            BarGoLink bar((int)result->GetRowCount());
            m_WardenPageChecksA.resize((int)result->GetRowCount());
            do
            {
                Field *fields = result->Fetch();
                bar.step();

                PageCheckEntry& current = m_WardenPageChecksA[count];
                current.Seed  = fields[0].GetUInt32();
                std::string res = fields[1].GetCppString();
                hexDecodeString(res.c_str(), 40, current.SHA);
                current.Offset  = fields[2].GetUInt32();
                current.Length  = fields[3].GetUInt8();

                ++count;
            } while(result->NextRow());
            delete result;
            sLog.outString();
            sLog.outString(">> Loaded %u page checks", count);
        }
    }
    // 2 bis) Page_B
    result = WorldDatabase.Query("SELECT Seed,SHA,Offset,Length FROM warden_check_page_b");
    if (!result)
    {
        sLog.outString(">> Table warden_check_page_b is empty!");
        sLog.outString();
    }
    else
    {
        uint32 count = 0;
        {
            BarGoLink bar((int)result->GetRowCount());
            m_WardenPageChecksB.resize((int)result->GetRowCount());
            do
            {
                Field *fields = result->Fetch();
                bar.step();

                PageCheckEntry& current = m_WardenPageChecksB[count];
                current.Seed  = fields[0].GetUInt32();
                std::string res = fields[1].GetCppString();
                hexDecodeString(res.c_str(), 40, current.SHA);
                current.Offset  = fields[2].GetUInt32();
                current.Length  = fields[3].GetUInt8();

                ++count;
            } while(result->NextRow());
            delete result;
            sLog.outString();
            sLog.outString(">> Loaded %u page checks", count);
        }
    }
    // 3) File
    result = WorldDatabase.Query("SELECT String,SHA FROM warden_check_mpq");
    if (!result)
    {
        sLog.outString(">> Table warden_check_mpq is empty!");
        sLog.outString();
    }
    else
    {
        uint32 count = 0;
        {
            BarGoLink bar((int)result->GetRowCount());
            m_WardenMPQChecks.resize((int)result->GetRowCount());
            do
            {
                Field *fields = result->Fetch();
                bar.step();

                MPQCheckEntry& current = m_WardenMPQChecks[count];
                current.String  = fields[0].GetCppString();
                std::string res = fields[1].GetCppString();
                hexDecodeString(res.c_str(), 40, current.SHA);

                ++count;
            } while(result->NextRow());
            delete result;
            sLog.outString();
            sLog.outString(">> Loaded %u file checks", count);
        }
    }
    // 4) Lua
    result = WorldDatabase.Query("SELECT String FROM warden_check_lua");
    if (!result)
    {
        sLog.outString(">> Table warden_check_lua is empty!");
        sLog.outString();
    }
    else
    {
        uint32 count = 0;
        {
            barGoLink bar((int)result->GetRowCount());
            m_WardenLuaChecks.resize((int)result->GetRowCount());
            do
            {
                Field *fields = result->Fetch();
                bar.step();

                LuaCheckEntry& current = m_WardenLuaChecks[count];
                current.String = fields[0].GetCppString();

                ++count;
            } while(result->NextRow());
            delete result;
            sLog.outString();
            sLog.outString(">> Loaded %u lua checks", count);
        }
    }
    // 5) Driver
    result = WorldDatabase.Query("SELECT Seed,SHA,String FROM warden_check_driver");
    if (!result)
    {
        sLog.outString(">> Table warden_check_driver is empty!");
        sLog.outString();
    }
    else
    {
        uint32 count = 0;
        {
            barGoLink bar((int)result->GetRowCount());
            m_WardenDriverChecks.resize((int)result->GetRowCount());
            do
            {
                Field *fields = result->Fetch();
                bar.step();

                DriverCheckEntry& current = m_WardenDriverChecks[count];
                current.Seed    = fields[0].GetUInt32();
                std::string res = fields[1].GetCppString();
                hexDecodeString(res.c_str(), 40, current.SHA);
                current.String  = fields[2].GetCppString();

                ++count;
            } while(result->NextRow());
            delete result;
            sLog.outString();
            sLog.outString(">> Loaded %u driver checks", count);
        }
    }
    // 2) dynamic memory
    result = WorldDatabase.Query("SELECT String,Offset,Length,Result,Comment FROM warden_check_memory_dynamic");
    if (!result)
    {
        sLog.outString(">> Table warden_check_memory_dynamic is empty!");
        sLog.outString();
    }
    else
    {
        uint32 count = 0;
        {
            BarGoLink bar((int)result->GetRowCount());
            m_WardenMemoryDynChecks.resize((int)result->GetRowCount());
            do
            {
                Field *fields = result->Fetch();
                bar.step();

                MemoryCheckEntry& current = m_WardenMemoryDynChecks[count];
                current.String  = fields[0].GetCppString();
                current.BaseOffset = fields[1].GetUInt32();
                current.Length  = fields[2].GetUInt8();
                std::string res = fields[3].GetCppString();
                current.Comment = fields[4].GetCppString();
                hexDecodeString(res.c_str(), res.length(), current.Result);

                ++count;
            } while(result->NextRow());
            delete result;
            sLog.outString();
            sLog.outString(">> Loaded %u memory checks", count);
        }
    }

    return true;
}

bool WardenMgr::CheckModuleExistOnDisk(const std::string &md5)
{
    std::string modulekey  = sWorld.GetDataPath()+ "warden/" + md5 + ".key";
    std::string modulefile = sWorld.GetDataPath()+ "warden/" + md5 + ".bin";
    FILE* mf = fopen(modulefile.c_str(), "rb");
    if (mf)
    {
        fclose(mf);
        mf = fopen(modulekey.c_str(), "rb");
        if (mf)
        {
            fclose(mf);
            return true;
        }
    }
    return false;
}

void WardenMgr::Register(WorldSession* const session)
{
    session->m_WardenTimer.SetInterval(2 * IN_MILLISECONDS);
    session->m_WardenTimer.Reset();
}

void WardenMgr::StartForSession(WorldSession* const session)
{
    if (!m_Enabled)
        return;

    if (session->m_wardenStatus != WARD_STATE_UNREGISTERED)
        return;

    std::string md5;
    std::string lastModule = "";
    // Check if the user already had the module same day
    time_t currenttime = time(NULL);
    tm* now = localtime(&currenttime);
    QueryResult* result = LoginDatabase.PQuery("SELECT module_day,last_module,os FROM account WHERE id = '%u'", session->GetAccountId());
    if (result)
    {
        Field* fields = result->Fetch();
        uint16 modDay = fields[0].GetUInt16();
        uint32 os = fields[2].GetUInt32();
        if (os != 0x0057696E)       // 0x0057696E = \0niW => 'Win' so not windows, not coded yet for macho modules sending
        {
            session->m_wardenStatus = WARD_STATE_USER_DISABLED;
            return;                 // OS not supported
        }

        if (modDay == now->tm_yday) // no need to change the module
        {
            lastModule = fields[1].GetCppString();
            if (lastModule.length()!=32)
            {
                DEBUG_LOG("Login same day, tried to get last used module failed, maybe never used warden");
                RandAModuleMd5(&md5);
            }
            else
            {
                md5 = lastModule;
            }
        }
        else
        {
            DEBUG_LOG("Login different day, so new warden module");
            RandAModuleMd5(&md5);
        }
        delete result;
        session->m_WardenModule = md5;
        if (md5 != lastModule)
            LoginDatabase.PExecute("UPDATE account SET last_module='%s',module_day=%u WHERE id = '%u'", md5.c_str(), now->tm_yday, session->GetAccountId());
        SendLoadModuleRequest(session);
        session->m_wardenStatus = WARD_STATE_LOAD_MODULE;
        session->m_WardenTimer.SetInterval(20 * IN_MILLISECONDS);
        session->m_WardenTimer.Reset();
    }
}

void WardenMgr::Unregister(WorldSession* const session)
{
    if (session->m_WardenClientChecks)
        delete (WardenClientCheckList*)session->m_WardenClientChecks;
}

void WardenMgr::RandAModuleMd5(std::string *result)
{
    std::vector<std::string> iList;
    for (WardenModuleMap::const_iterator itr = m_WardenModuleMap.begin(); itr != m_WardenModuleMap.end(); ++itr)
    {
        iList.push_back(itr->first);
    }
    uint8 choice = urand(0, iList.size()-1);
    *result = iList[choice];
}

void WardenMgr::SendLoadModuleRequest(WorldSession* const session)
{
    std::string modulekeyfile = sWorld.GetDataPath()+ "warden/" + session->m_WardenModule + ".key";

    // Load .key file to get module length and module key
    FILE* mf = fopen(modulekeyfile.c_str(), "rb");
    if (!mf)
        return; // If this happens, this mean the user deleted the file after startup

    uint32 mod_length;
    uint8 rc4[16];
    uint8 binMd5[16];
    hexDecodeString(session->m_WardenModule.c_str(), 32, binMd5);

    fread(&mod_length, 1, 4, mf);
    fread(rc4, sizeof(uint8)*16, 1, mf);
    fclose(mf);

    WorldPacket data( SMSG_WARDEN_DATA, 1+16+16+4 );
    data << uint8(WARDS_MODULE_INFO);
    data.append(binMd5, 16);
    data.append(rc4, 16);
    data << uint32(mod_length);

    uint8 *skey = session->m_Socket->GetSessionKey().AsByteArray(40);
    sWardenMgr.SetInitialKeys(&skey[0], &skey[20], &session->m_rc4ClientKey[0], &session->m_rc4ServerKey[0]);

    // Then send the first packet to client
    data.crypt(&session->m_rc4ServerKey[0], &rc4_crypt);
    session->SendPacket(&data);
}

void WardenMgr::SendModule(WorldSession* const session)
{
    std::string modulekeyfile = sWorld.GetDataPath()+ "warden/" + session->m_WardenModule + ".key";
    std::string modulefile = sWorld.GetDataPath()+ "warden/" + session->m_WardenModule + ".bin";

    // Load .key file to get module length and module key
    FILE* mf = fopen(modulekeyfile.c_str(), "rb");
    if (!mf)
        return; // Should not happen

    uint32 modLength, remainLen;
    uint8 rc4[16];
    fread(&modLength, 1, 4, mf);
    fread(rc4, sizeof(uint8)*16, 1, mf);
    fclose(mf);

    // Load the module encrypted binary
    mf = fopen(modulefile.c_str(), "rb");
    if (!mf)
        return; // Should not happen

    uint8* m_tmpModule;
    uint16 offset = 0;
    m_tmpModule = (uint8*)malloc(sizeof(uint8)*modLength);
    fread(m_tmpModule, sizeof(uint8)*modLength, 1, mf);
    fclose(mf);
    remainLen = modLength;
    while (remainLen > 0)
    {
        uint16 len = remainLen>500?500:remainLen;
        WorldPacket data( SMSG_WARDEN_DATA, 1+2+len );
        data << uint8(WARDS_MODULE_CHUNK);
        data << uint16(len);
        data.append(m_tmpModule + offset, len);
        offset = offset + len;
        remainLen = remainLen - len;

        data.crypt(&session->m_rc4ServerKey[0], &rc4_crypt);
        session->SendPacket(&data);
    }
    free(m_tmpModule);
}

void WardenMgr::SendSeedTransformRequest(WorldSession* const session)
{
    DEBUG_LOG("WardenMgr::SendSeedTransformRequest: Client packet");
    WorldPacket data( SMSG_WARDEN_DATA, 1+16 );
    data << uint8(WARDS_SEED);
    data.append(&session->m_wardenSeed[0], 16);
    data.crypt(&session->m_rc4ServerKey[0], &rc4_crypt);
    session->SendPacket(&data);
    session->m_wardenStatus = WARD_STATE_TRANSFORM_SEED;
    session->m_WardenTimer.SetInterval(20 * IN_MILLISECONDS);
    session->m_WardenTimer.Reset();
}

void WardenMgr::SendSeedAndComputeKeys(WorldSession* const session)
{
    DEBUG_LOG("WardenMgr::SendSeedAndComputeKeys: building wardend packet");
    BigNumber s;
    s.SetRand(16 * 8);
    // save this seed for client send later when we have the new keys from wardend
    memcpy(&session->m_wardenSeed[0], s.AsByteArray(16), 16);
    // build the packet for wardend only
    LoadModuleAndGetKeys(session);

    // And we send this packet to the warden daemon for it to make the new key pair
    session->m_wardenStatus = WARD_STATE_PENDING_WARDEND;
}

void WardenMgr::LoadModuleAndGetKeys(WorldSession* const session)
{
    if (!m_WardenProcessStream)
        return;

    std::string modulekeyfile = sWorld.GetDataPath()+ "warden/" + session->m_WardenModule + ".key";
    std::string modulefile = sWorld.GetDataPath()+ "warden/" + session->m_WardenModule + ".bin";

    // Load .key file to get module length and module key
    FILE* mf = fopen(modulekeyfile.c_str(), "rb");
    if (!mf)
        return; // Modules have been tested at WardenMgr init, so have been deleted while core was running !

    uint32 modLength;
    uint8 rc4[16];
    fread(&modLength, 1, 4, mf);
    fread(rc4, sizeof(uint8)*16, 1, mf);
    fclose(mf);

    // Load the module encrypted binary
    mf = fopen(modulefile.c_str(), "rb");
    if (!mf)
        return; // Same as above

    uint8* m_tmpModule = (uint8*)malloc(sizeof(uint8)*modLength);
    fread(m_tmpModule, sizeof(uint8)*modLength, 1, mf);
    fclose(mf);

    // Just decrypt it so that we don't even need to send the rc4
    uint8 m_tmpKey[0x102];
    rc4_init(m_tmpKey, rc4, 16);
    rc4_crypt(m_tmpKey, m_tmpModule, modLength);

    uint32 m_signature = *(uint32*)(m_tmpModule + modLength - 0x100 - 4); // - 256 bytes - sizeof(uint32)
    if (m_signature != 0x5349474E) // NGIS->SIGN string
    {
        BASIC_LOG("Module damaged on disk");
        return;
    }

    ByteBuffer pkt;
    pkt << uint8(MMSG_LOAD_MODULE);
    pkt << uint32(modLength - 0x100); // - 256 bytes certificate

    pkt << uint32(session->GetAccountId());
    pkt.append(m_tmpModule, modLength - 0x100);

    pkt.append(session->m_Socket->GetSessionKey().AsByteArray(40), 40);
    // Same as when we send this transformed seed request to client
    pkt << uint8(WARDS_SEED);
    pkt.append(&session->m_wardenSeed[0], 16);
    free(m_tmpModule);

    m_WardenProcessStream->send((char const*)pkt.contents(), pkt.size());
}

void WardenMgr::Pong()
{
    m_PingTimer.Reset();
    m_PingOut = false;
}

void WardenMgr::SendPing()
{
    ByteBuffer pkt;
    pkt << uint8(MMSG_PING);
    m_WardenProcessStream->send((char const*)pkt.contents(), pkt.size());
    m_PingOut = true;
}

void WardenMgr::SetInitialKeys(const uint8 *bSessionKey1, const uint8 *bSessionKey2, uint8* ClientKey, uint8 *ServerKey)
{
     uint8 bRandomData[20] = {0};
     uint8 bRandomSource1[20] = {0};
     uint8 bRandomSource2[20] = {0};

     SHA1(bSessionKey1, 20, bRandomSource1);
     SHA1(bSessionKey2, 20, bRandomSource2);

     SHA_CTX mCtx;
     SHA1_Init(&mCtx);

     // compute hash
     SHA1_Update(&mCtx, bRandomSource1, 20);
     SHA1_Update(&mCtx, bRandomData, 20);
     SHA1_Update(&mCtx, bRandomSource2, 20);

     // finalize
     SHA1_Final(bRandomData, &mCtx);

     uint8 position = 0;
     uint8 key[16] = {0};
     for (uint8 i=0; i<16; ++i)
     {
         if (position >= 20)
         {
             SHA1_Init(&mCtx);
             SHA1_Update(&mCtx, bRandomSource1, 20);
             SHA1_Update(&mCtx, bRandomData, 20);
             SHA1_Update(&mCtx, bRandomSource2, 20);
             SHA1_Final(bRandomData, &mCtx);
             position = 0;
         }
         key[i] = bRandomData[position];
         ++position;
     }
     rc4_init(ClientKey, key, 16);

     for (uint8 i=0; i<16; ++i)
         key[i]=0;

     for (uint8 i=0; i<16; ++i)
     {
         if (position >= 20)
         {
             SHA1_Init(&mCtx);
             SHA1_Update(&mCtx, bRandomSource1, 20);
             SHA1_Update(&mCtx, bRandomData, 20);
             SHA1_Update(&mCtx, bRandomSource2, 20);
             SHA1_Final(bRandomData, &mCtx);
             position = 0;
         }
         key[i] = bRandomData[position];
         ++position;
     }
     rc4_init(ServerKey, key, 16);
}

void WardenMgr::ChangeClientKey(WorldSession* const session)
{
    DEBUG_LOG("WardenMgr::ChangeClientKey");
    memcpy(&session->m_rc4ClientKey[0], &session->m_WardenTmpClientKey[0], 0x102);
}

// Sending this packet to initialize engine functions warden uses
void WardenMgr::SendWardenData(WorldSession* const session)
{
    DEBUG_LOG("WardenMgr::SendWardenData");
    WorldPacket data(SMSG_WARDEN_DATA, 1+2+4+20 + 1+2+4+8 + 1+2+4+8); // 57 // 1.12.1 init packet
    {
        data << uint8(WARDS_DATA);
        data << uint16(20);
        uint8 buff[20] =
        {
            0x01, 0x00, 0x02, 0x00,
            0x80, 0x4F, 0x02, 0x00,     // 0x00400000 + 0x00024F80 SFileOpenFile
            0xC0, 0x18, 0x02, 0x00,     // 0x00400000 + 0x000218C0 SFileGetFileSize
            0x30, 0x25, 0x02, 0x00,     // 0x00400000 + 0x00022530 SFileReadFile
            0x10, 0x29, 0x02, 0x00      // 0x00400000 + 0x00022910 SFileCloseFile
        };
        data << uint32(BuildChecksum(buff, 20));
        data.append(buff, 20);
    }
    {
        data << uint8(WARDS_DATA);
        data << uint16(8);
        uint8 buff[8] =
        {
           0x04, 0x00, 0x00,
           0x40, 0x9D, 0x41, 0x00,      // 0x00400000 + 0x00419D40 FrameScript__GetText
           0x01
        };
        data << uint32(BuildChecksum(buff, 8));
        data.append(buff, 8);
    }
    // Timing checks
    {
        data << uint8(WARDS_DATA);
        data << uint16(8);
        uint8 buff[8] =
        {
           0x01, 0x01, 0x00,
           0x20, 0xAE, 0x46, 0x00,      // 0x00400000 + 0x0046AE20 PerformanceCounter
           0x01
        };
        data << uint32(BuildChecksum(buff, 8));
        data.append(buff, 8);
    }

    data.crypt(&session->m_rc4ServerKey[0], &rc4_crypt);
    session->SendPacket(&data);
}

uint32 WardenMgr::BuildChecksum(const uint8* data, uint32 dataLen)
{
    uint8 hash[20];
    SHA1(data, dataLen, hash);
    uint32 checkSum = 0;
    for (uint8 i=0; i<5; ++i)
        checkSum = checkSum ^ *(uint32*)(&hash[0] + i*4);
    return checkSum;
}

bool WardenMgr::ValidateTSeed(WorldSession* const session, const uint8 *codedClientTSeed)
{
    uint8 codedServerTSeed[20];
    SHA1(&session->m_wardenSeed[0], 16, &codedServerTSeed[0]);
    if (memcmp(&codedServerTSeed[0], codedClientTSeed, 20))
    {
        ReactToCheatCheckResult(session, false);
        return false;
    }
    return true;
}

WardenMgr::MemoryCheckEntry *WardenMgr::GetRandMemCheck()
{
    return &m_WardenMemoryChecks[urand(0, m_WardenMemoryChecks.size()-1)];
}
WardenMgr::MemoryCheckEntry *WardenMgr::GetRandMemDynCheck()
{
    return &m_WardenMemoryDynChecks[urand(0, m_WardenMemoryDynChecks.size()-1)];
}
WardenMgr::PageCheckEntry *WardenMgr::GetRandPageCheck(uint8 type)
{
    if (type == WARD_CHECK_PAGE_A)
        return &m_WardenPageChecksA[urand(0, m_WardenPageChecksA.size()-1)];
    else
        return &m_WardenPageChecksB[urand(0, m_WardenPageChecksB.size()-1)];
}
WardenMgr::MPQCheckEntry *WardenMgr::GetRandMPQCheck()
{
    return &m_WardenMPQChecks[urand(0, m_WardenMPQChecks.size()-1)];
}
WardenMgr::LuaCheckEntry *WardenMgr::GetRandLuaCheck()
{
    return &m_WardenLuaChecks[urand(0, m_WardenLuaChecks.size()-1)];
}
WardenMgr::DriverCheckEntry *WardenMgr::GetRandDriverCheck()
{
    return &m_WardenDriverChecks[urand(0, m_WardenDriverChecks.size()-1)];
}

void WardenMgr::SendCheatCheck(WorldSession* const session)
{
    DEBUG_LOG("Wardend::SendCheatCheck(%u, *pkt)", session->GetAccountId());

    std::string md5 = session->m_WardenModule;
    if (!session->m_WardenClientChecks)
    {
        session->m_WardenClientChecks = new WardenClientCheckList;
    }
    // Type cast and get a shorter name
    WardenClientCheckList* checkList = (WardenClientCheckList*)session->m_WardenClientChecks;

    checkList->clear();
    // Get the Seed 1st byte for the xoring
    uint8 m_seed1 = session->m_wardenSeed[0];
    DEBUG_LOG("Seed byte: 0x%02X, end byte: 0x%02X", m_seed1, m_WardenModuleMap[md5][WARD_CHECK_END]);

    WorldPacket data( SMSG_WARDEN_DATA, 300 ); // Guess size
    data << uint8(WARDS_CHEAT_CHECK);

    // Rand a number of checks between 4 and 8 checks + the first time check + end packet
    uint8 nbChecks = 0; //urand(4, 8);
    checkList->resize(nbChecks);

    for (uint8 i=0; i<nbChecks; ++i)
    {
        // We select one based on the ratio
        float mRand = 99.0f; //rand_chance_f();
        if (mRand < WCHECK_PAGE_B_RATIO)                 // size 29, no string both page1 and page2 tests
        {
            (*checkList)[i].check = urand(0,1)?WARD_CHECK_PAGE_A:WARD_CHECK_PAGE_B;
            (*checkList)[i].page = GetRandPageCheck((*checkList)[i].check);
        }
        else if (mRand < WCHECK_MEMORY_RATIO)           // size 6, possible string
        {
            (*checkList)[i].check = WARD_CHECK_MEMORY;
            (*checkList)[i].mem = GetRandMemCheck();
            if ((*checkList)[i].mem->String.length())   // add 1 for the uint8 str length
            {
                data << uint8((*checkList)[i].mem->String.length());
                data.append((*checkList)[i].mem->String.c_str() ,(*checkList)[i].mem->String.length());
                DEBUG_LOG("Mem str %s, len %u", (*checkList)[i].mem->String.c_str(), (*checkList)[i].mem->String.length());
            }
        }
        else if (mRand < WCHECK_DRIVER_RATIO)
        {
            (*checkList)[i].check = WARD_CHECK_DRIVER;  // size 25 + string
            (*checkList)[i].driver = GetRandDriverCheck();
            data << uint8((*checkList)[i].driver->String.length());
            data.append((*checkList)[i].driver->String.c_str(), (*checkList)[i].driver->String.length());
            DEBUG_LOG("Driver str %s, len %u", (*checkList)[i].driver->String.c_str(), (*checkList)[i].driver->String.length());
        }
        else if (mRand < WCHECK_MPQ_RATIO)
        {
            (*checkList)[i].check = WARD_CHECK_MPQ;    // size 1 + string
            (*checkList)[i].file = GetRandMPQCheck();
            data << uint8((*checkList)[i].file->String.length());
            data.append((*checkList)[i].file->String.c_str(), (*checkList)[i].file->String.length());
            DEBUG_LOG("File str %s, len %u", (*checkList)[i].file->String.c_str(), (*checkList)[i].file->String.length());
        }
        else
        {
            (*checkList)[i].check = WARD_CHECK_LUA;     // size 1 + string
            (*checkList)[i].lua = GetRandLuaCheck();
            data << uint8((*checkList)[i].lua->String.length());
            data.append((*checkList)[i].lua->String.c_str(), (*checkList)[i].lua->String.length());
            DEBUG_LOG("Lua str %s, len %u", (*checkList)[i].lua->String.c_str(), (*checkList)[i].lua->String.length());
        }
    }
    // Strings terminator
    data << uint8(0);
    // We first add a timing check
    data << uint8(m_WardenModuleMap[md5][WARD_CHECK_TIMING] ^ m_seed1);
    // Finaly put the other checks
    uint8 m_strIndex = 1;
    DEBUG_LOG("Preparing %u checks", nbChecks);
    for (uint8 i=0; i<nbChecks; ++i)
    {
        data << uint8(m_WardenModuleMap[md5][(*checkList)[i].check] ^ m_seed1);
        switch ((*checkList)[i].check)
        {
            case WARD_CHECK_PAGE_A:
            case WARD_CHECK_PAGE_B:
                DEBUG_LOG("%u : %s", i, (*checkList)[i].check==WARD_CHECK_PAGE_A?"WARD_CHECK_PAGE_A":"WARD_CHECK_PAGE_B");
                data << uint32((*checkList)[i].page->Seed);
                data.append(&(*checkList)[i].page->SHA[0], 20);
                data << uint32((*checkList)[i].page->Offset);
                data << uint8((*checkList)[i].page->Length);
                break;
            case WARD_CHECK_MEMORY:
                DEBUG_LOG("%u : WARD_CHECK_MEMORY", i);
                if ((*checkList)[i].mem->String.length())
                    data << uint8(m_strIndex++);
                else
                    data << uint8(0);
                data << uint32((*checkList)[i].mem->Offset);
                data << uint8((*checkList)[i].mem->Length);
                break;
            case WARD_CHECK_DRIVER:
                DEBUG_LOG("%u : WARD_CHECK_DRIVER", i);
                data << uint32((*checkList)[i].driver->Seed);
                data.append(&(*checkList)[i].driver->SHA[0], 20);
                data << uint8(m_strIndex++);
                break;
            case WARD_CHECK_MPQ:
                DEBUG_LOG("%u : WARD_CHECK_FILE", i);
                data << uint8(m_strIndex++);
                break;
            case WARD_CHECK_LUA:
                DEBUG_LOG("%u : WARD_CHECK_LUA", i);
                data << uint8(m_strIndex++);
                break;
        }
    }
    data << uint8(m_WardenModuleMap[md5][WARD_CHECK_END] ^ m_seed1);

    data.hexlike();
    data.crypt(&session->m_rc4ServerKey[0], &rc4_crypt);
    session->SendPacket(&data);
}

bool WardenMgr::ValidateCheatCheckResult(WorldSession* const session, WorldPacket& clientPacket)
{
    uint32 accountId = session->GetAccountId();
    DEBUG_LOG("Wardend::ValidateCheatCheckResult(%u, *pkt)", accountId);
    bool valid = true;

    uint16 pktLen;
    uint32 checksum;
    clientPacket >> pktLen;
    clientPacket >> checksum;
    if (checksum != BuildChecksum(clientPacket.contents() + clientPacket.rpos(), clientPacket.size() - clientPacket.rpos()))
    {
        BASIC_LOG("Warden Cheat-check: Kicking account %u for failed check, Packet Checksum 0x%08X is invalid!", session->GetAccountId(), checksum);
        ReactToCheatCheckResult(session, false);
        return false;
    }
    if (pktLen==0)
    {
        DEBUG_LOG("Packet is 0 length");
        session->m_BanRaison = "Warden error packet";
        return false;
    }

    bool localCheck = true;
    // parse the timing check always sent
    DEBUG_LOG("TimeCheck");
    uint8 res;
    uint32 clientTicks;
    clientPacket >> res; // should be 1
    if (res == 0)
    {
        session->m_BanRaison = "Timing check returned an error";
        DEBUG_LOG("  Timing check returned 0 result");
        localCheck = false;
    }
    pktLen = pktLen - 5;

    WardenClientCheckList* checkList = (WardenClientCheckList*)session->m_WardenClientChecks;
    if (!checkList)
        return false;

    for (uint8 i=0; i<checkList->size(); ++i)
    {
        switch ((*checkList)[i].check)
        {
            // case WARD_CHECK_TIMING: already managed and we don't experct more than one
            case WARD_CHECK_MEMORY:
            {
                bool currentCheckValid = true;
                DEBUG_LOG("MemCheck");
                uint8 res;
                clientPacket >> res; // should be 0
                if (res)
                {
                    localCheck = false;
                    currentCheckValid = false;
                    BASIC_LOG("  Kicking account %u for failed check, MEM at Offset 0x%04X, lentgh %u could not be read by client", accountId, (*checkList)[i].mem->Offset, (*checkList)[i].mem->Length);
                }
                else
                {
                    uint8 memContent[20];
                    for (uint8 pos=0; pos<(*checkList)[i].mem->Length; ++pos)
                        clientPacket >> memContent[pos];
                    if (memcmp(&memContent[0], &(*checkList)[i].mem->Result[0], (*checkList)[i].mem->Length))
                    {
                        //localCheck = false;
                        std::string strContent, strContent2, strContent3;
                        hexEncodeByteArray(memContent, (*checkList)[i].mem->Length, strContent);
                        hexEncodeByteArray((*checkList)[i].mem->Result, (*checkList)[i].mem->Length, strContent2);
                        strContent3 = (*checkList)[i].mem->Comment.size() ? string_format(" (%s)", (*checkList)[i].mem->Comment.c_str()) : "";
                        session->m_BanRaison = string_format("Failed MEM check Offset 0x%04X length %u content '%s' instead of '%s'%s", 
                            (*checkList)[i].mem->Offset, (*checkList)[i].mem->Length, strContent.c_str(), strContent2.c_str(), strContent3.c_str());
                        BASIC_LOG("  Kicking account %u for failed check, MEM Offset 0x%04X length %u has content '%s' instead of '%s'",
                            accountId, (*checkList)[i].mem->Offset, (*checkList)[i].mem->Length, strContent.c_str(), strContent2.c_str());
                    }
                    pktLen = pktLen - (1 + (*checkList)[i].mem->Length);
                }
                DEBUG_LOG("  Mem %s",currentCheckValid?"Ok":"Failed");
                break;
            }
            case WARD_CHECK_MPQ:
            {
                DEBUG_LOG("MPQCheck");
                bool currentCheckValid = true;
                uint8 res;
                uint8 resSHA1[20];
                clientPacket >> res; // should be 0
                if (res)
                {
                    currentCheckValid = false;
                    localCheck = false;
                    BASIC_LOG("  Kicking account %u for failed check, MPQ '%s' not found by client", accountId, (*checkList)[i].file->String.c_str());
                    pktLen = pktLen - 1;
                }
                else
                {
                    for (uint8 pos=0; pos<20; ++pos)
                        clientPacket >> resSHA1[pos];
                    if (res || memcmp(resSHA1, (*checkList)[i].file->SHA, 20))
                    {
                        currentCheckValid = false;
                        localCheck = false;
                        std::string strResSHA1, strReqSHA1;
                        hexEncodeByteArray(resSHA1, 20, strResSHA1);
                        hexEncodeByteArray((*checkList)[i].file->SHA, 20, strReqSHA1);
                        session->m_BanRaison = string_format("Failed MPQ check '%s' SHA1 is '%s' instead of '%s'", (*checkList)[i].file->String.c_str(), strResSHA1.c_str(), strReqSHA1.c_str());
                        BASIC_LOG("  Kicking account %u for failed check, MPQ '%s' SHA1 is '%s' instead of '%s'", accountId, (*checkList)[i].file->String.c_str(), strResSHA1.c_str(), strReqSHA1.c_str());
                    }
                    pktLen = pktLen - 21;
                }
                DEBUG_LOG("  MPQ %s",currentCheckValid?"Ok":"Failed");
                break;
            }
            case WARD_CHECK_LUA:
            {
                DEBUG_LOG("LUACheck");
                bool currentCheckValid = true;
                uint8 res;
                uint8 foundLuaLen;
                clientPacket >> res; // should be 0
                if (res == 1)
                {
                    DEBUG_LOG("  LUA check Ok");
                    continue;
                }
                clientPacket >> foundLuaLen; // should be 0
                uint8 *luaStr;
                if (foundLuaLen > 0)
                {
                    luaStr = (uint8*)malloc(foundLuaLen+1);
                    for (uint8 pos=0; pos<foundLuaLen; ++pos)
                    {
                        clientPacket >> luaStr[pos];
                    }
                    luaStr[foundLuaLen] = 0;
                    session->m_BanRaison = string_format("Failed Lua check for '%s', '%s' found instead", (*checkList)[i].lua->String.c_str(), (char*)luaStr);
                    BASIC_LOG("  Kicking account %u for failed check, Lua '%s' found as '%s'", accountId, (*checkList)[i].lua->String.c_str(), (char*)luaStr);
                    localCheck = false;
                    currentCheckValid = false;
                    free(luaStr);
                }
                DEBUG_LOG("  Lua %s",currentCheckValid?"Ok":"Failed");
                pktLen = pktLen - 2;
                break;
            }
            case WARD_CHECK_PAGE_A:
            case WARD_CHECK_PAGE_B:
            case WARD_CHECK_DRIVER:
            {
                DEBUG_LOG("PageCheck or DriverCheck");
                bool currentCheckValid = true;
                uint8 res;
                clientPacket >> res; // should be 0xE9
                if (res != 0xE9)
                {
                    if ((*checkList)[i].check == WARD_CHECK_DRIVER)
                    {
                        session->m_BanRaison = string_format("Failed driver check '%s'", (*checkList)[i].driver->String.c_str());
                        BASIC_LOG("  Kicking account %u for failed driver check '%s'", accountId ,(*checkList)[i].driver->String.c_str());
                    }
                    else
                    {
                        session->m_BanRaison = string_format("Failed page check offset 0x%04X, length %u", (*checkList)[i].page->Offset, (*checkList)[i].page->Length);
                        BASIC_LOG("  Kicking account %u for failed page check Offset 0x%08X, length %u", accountId, (*checkList)[i].page->Offset, (*checkList)[i].page->Length);
                    }
                    localCheck = false;
                    currentCheckValid = false;
                }
                DEBUG_LOG("  Page or Driver %s",currentCheckValid?"Ok":"Failed");
                pktLen = pktLen - 1;
                break;
            }
            default:
                DEBUG_LOG("Other!!");
                // Finish skiping the rest of the packet and return failed checks
                session->m_BanRaison = string_format("Invalid Warden packet , %u bytes could not be parsed", clientPacket.size() - clientPacket.rpos());
                BASIC_LOG("  Wrong packet for account %u or problem to parse it, I had to clean %u bytes", accountId, clientPacket.size() - clientPacket.rpos());
                clientPacket.read_skip(clientPacket.size() - clientPacket.rpos());
                return false;
        }
        valid = valid && localCheck;
    }
    return valid;
}

void WardenMgr::ReactToCheatCheckResult(WorldSession* const session, bool result)
{
    DEBUG_LOG("ReactToCheatCheckResult %s",result?"true":"false");
    if (result)
    {
        session->m_wardenStatus = WARD_STATE_CHEAT_CHECK_IN;
        const uint32 shortTime = urand(25, 35);                 // from 25 to 35 seconds
        session->m_WardenTimer.SetInterval(shortTime * IN_MILLISECONDS);
        DEBUG_LOG("Timer set to %u seconds", shortTime);
        session->m_WardenTimer.SetCurrent(0);                   // Full time choosen
    }
    else
    {
        if (session->_player)
            session->_player->SaveToDB();
        if (m_Banning)
            sWorld.BanAccount(session, sWorld.getConfig(CONFIG_UINT32_WARDEN_BAN_TIME) * 24 * HOUR, session->m_BanRaison.c_str(), "Warden System");
        else
            session->KickPlayer();
    }
}

// String format fonction, not related to Warden but useful
std::string WardenMgr::string_format(const std::string fmt_str, ...) {
    int final_n, n = ((int)fmt_str.size()) * 2; /* reserve 2 times as much as the length of the fmt_str */
    std::string str;
    std::unique_ptr<char[]> formatted;
    va_list ap;
    while(1) {
        formatted.reset(new char[n]); /* wrap the plain char array into the unique_ptr */
        strcpy(&formatted[0], fmt_str.c_str());
        va_start(ap, fmt_str);
        final_n = vsnprintf(&formatted[0], n, fmt_str.c_str(), ap);
        va_end(ap);
        if (final_n < 0 || final_n >= n)
            n += abs(final_n - n + 1);
        else
            break;
    }
    return std::string(formatted.get());
}

/////////////////////////////
// Session
/////////////////////////////
void WorldSession::HandleWardenRegister()
{
    if (sWardenMgr.IsEnabled() && m_wardenStatus == WARD_STATE_UNREGISTERED)
        sWardenMgr.Register(this);
}

void WorldSession::HandleWardenUnregister()
{
    if (sWardenMgr.IsEnabled())
        sWardenMgr.Unregister(this);
}

void WorldSession::HandleWardenDataOpcode(WorldPacket& recv_data)
{
    if (sWardenMgr.IsEnabled())
    {
        DETAIL_LOG("WARDEN: Receiver opcode CMSG_WARDEN_DATA");
        recv_data.crypt(m_rc4ClientKey, &rc4_crypt);
        uint8 warden_opcode;
        recv_data >> warden_opcode;
        //recv_data.hexlike();
        switch(warden_opcode)
        {
            case WARDC_MODULE_LOAD_FAILED:
                DETAIL_LOG("WARDEN: sub-opcode WARDC_MODULE_LOAD_FAILED");
                // We have to send the module
                if (m_wardenStatus == WARD_STATE_LOAD_FAILED)
                {
                    KickPlayer();
                }
                else
                {
                    sWardenMgr.SendModule(this);
                    m_wardenStatus = WARD_STATE_LOAD_FAILED;
                    m_WardenTimer.SetInterval(30 * IN_MILLISECONDS);
                    m_WardenTimer.Reset();
                }
                break;
            case WARDC_MODULE_LOADED:
                DETAIL_LOG("WARDEN: sub-opcode WARDC_MODULE_LOADED");
                // We go next step: Send a seed
                sWardenMgr.SendSeedAndComputeKeys(this);
                m_WardenTimer.SetInterval(5 * IN_MILLISECONDS);
                m_WardenTimer.Reset();
                break;
            case WARDC_CHEAT_CHECK_RESULT:
            {
                DETAIL_LOG("WARDEN: sub-opcode WARDC_CHEAT_CHECK_RESULT");
                bool result = sWardenMgr.ValidateCheatCheckResult(this, recv_data);
                sWardenMgr.ReactToCheatCheckResult(this, result);   // This sets the timer if needed
                break;
            }
            case WARDC_TRANSFORMED_SEED:
                DETAIL_LOG("WARDEN: sub-opcode WARDC_TRANSFORMED_SEED");
                // Let's validate this data
                if (sWardenMgr.ValidateTSeed(this, recv_data.contents()+recv_data.rpos()))
                {
                    sWardenMgr.ChangeClientKey(this);
                    sWardenMgr.SendWardenData(this);
                    m_wardenStatus = WARD_STATE_CHEAT_CHECK_IN;
                    m_WardenTimer.SetInterval(3 * IN_MILLISECONDS); // 3 secs before the 1st cheat check
                    m_WardenTimer.Reset();
                }
                recv_data.read_skip(20);
                break;
            default:
                error_log("Problem with WARDEN packet");
        }
    }
    else
    {
        recv_data.read_skip<uint8>();
        DEBUG_LOG("Received opcode CMSG_WARDEN_DATA, but this session has no module linked");
    }
}

//****************************************************
// Warden Demon replies handlers

const WardenSvcHandler::WardenMgrCmd table[] =
{
    { WMSG_WARDEN_KEYS,                 &WardenSvcHandler::_HandleNewKeys                   },
    { WMSG_PONG,                        &WardenSvcHandler::_HandlePong                      },
    { WMSG_CONNECTION_END,              &WardenSvcHandler::_HandleDisconnect                }
};

#define WARDEN_REPLY_TOTAL_COMMANDS sizeof(table)/sizeof(WardenMgrCmd)

int WardenSvcHandler::open(void*)
{
    BASIC_LOG("Connection to Warden Daemon established");
    ACE_Reactor::instance()->register_handler(this, ACE_Event_Handler::READ_MASK);
    Peer=&peer();
    return 0;
}

int WardenSvcHandler::handle_input(ACE_HANDLE /*handle*/)
{
    uint8 _cmd;
    Peer->recv_n(&_cmd, 1);
    bool _valid = false;

    for (int i = 0; i < WARDEN_REPLY_TOTAL_COMMANDS; ++i)
    {
        if ((uint8)table[i].cmd == _cmd)
        {
            _valid = true;
            if (!(*this.*table[i].handler)())
                return 0;
            break;
        }
    }
    if (!_valid) // Empty the queue
    {
        uint8 _trash[1024];
        sLog.outError("Unexpected packet [%u] from Wardend, trashing it", _cmd);
        Peer->recv_n(_trash, 1024);
    }
    return 0;
}

bool WardenSvcHandler::_HandleNewKeys()
{
    DEBUG_LOG("WardenSvcHandler::_HandleNewKeys()");
    uint32 accountId;
    Peer->recv_n(&accountId, 4);
    WorldSession* session = sWorld.FindSession(accountId);
    if (session) // in case client disconnected in between
    {
        // 1st, send the transformed seed request to client
        sWardenMgr.SendSeedTransformRequest(session);
        // now we can change the server key
        Peer->recv_n(session->GetWardenServerKey(), 0x102);
        // But we need a Transformed seed reply from client before changing the client key
        // So saving it
        Peer->recv_n(session->GetWardenTempClientKey(), 0x102);
        // The seed can be overwritten since we did use the original one to build the client request
        Peer->recv_n(session->GetWardenSeed(), 16);
    }
    else
    {
        // Trash the packet
        uint8 trash;
        for (uint32 i=0; i<(0x102+0x102+16); ++i)
        {
            Peer->recv_n(&trash, 1);
        }
    }
    return true;
}

bool WardenSvcHandler::_HandlePong()
{
    sWardenMgr.Pong();
    return true;
}

bool WardenSvcHandler::_HandleDisconnect()
{
    sWardenMgr.SetDisconnected();
    return true;
}

INSTANTIATE_SINGLETON_1(WardenMgr);