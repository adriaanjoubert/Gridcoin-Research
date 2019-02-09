/* scraper_net.cpp */

/* Define this if you want to show pubkey as address, otherwise hex id */
#define SCRAPER_NET_PK_AS_ADDRESS

#include <memory>
#include "net.h"
#include "rpcserver.h"
#include "rpcprotocol.h"
#ifdef SCRAPER_NET_PK_AS_ADDRESS
#include "base58.h"
#endif
#include "scraper_net.h"
#include "appcache.h"

//Globals
std::map<uint256,CSplitBlob::CPart> CSplitBlob::mapParts;
std::map< uint256, std::unique_ptr<CScraperManifest> > CScraperManifest::mapManifest;
CCriticalSection CScraperManifest::cs_mapManifest;
extern unsigned int SCRAPER_MISBEHAVING_NODE_BANSCORE;

bool CSplitBlob::RecvPart(CNode* pfrom, CDataStream& vRecv)
{
    /* Part of larger hashed blob. Currently only used for scraper data sharing.
   * retrive parent object from mapBlobParts
   * notify object or ignore if no object found
   * erase from mapAlreadyAskedFor
  */
    auto& ss= vRecv;
    uint256 hash(Hash(ss.begin(), ss.end()));
    mapAlreadyAskedFor.erase(CInv(MSG_PART,hash));

    auto ipart= mapParts.find(hash);

    if(ipart!=mapParts.end())
    {
        CPart& part= ipart->second;
        assert(vRecv.size()>0);
        if(!part.present())
        {
            LogPrint("manifest", "received part %s %u refs", hash.GetHex(),(unsigned)part.refs.size());
            part.data= CSerializeData(vRecv.begin(),vRecv.end()); //TODO: replace with move constructor
            for( const auto& ref : part.refs )
            {
                CSplitBlob& split= *ref.first;
                ++split.cntPartsRcvd;
                assert(split.cntPartsRcvd <= split.vParts.size());
                if( split.isComplete() )
                {
                    split.Complete();
                }
            }
            return true;
        } else {
            LogPrint("manifest", "received duplicate part %s", hash.GetHex());
            return false;
        }
    } else {
        if(pfrom)
        {
            pfrom->Misbehaving(SCRAPER_MISBEHAVING_NODE_BANSCORE / 5);
            LogPrintf("WARNING: CSplitBlob::RecvPart: Spurious part received from %s. Adding %u banscore.",
                     pfrom->addr.ToString(), SCRAPER_MISBEHAVING_NODE_BANSCORE / 5);
        }
        return error("Spurious part received!");
    }
}

void CSplitBlob::addPart(const uint256& ihash)
{
    assert( ihash != Hash(vParts.end(),vParts.end()) );
    unsigned n= vParts.size();
    auto rc= mapParts.emplace(ihash,CPart(ihash));
    CPart& part= rc.first->second;
    /* add to local vector */
    vParts.push_back(&part);
    if(part.present())
        cntPartsRcvd++;
    /* nature of set ensures no duplicates */
    part.refs.emplace(this, n);
}

int CSplitBlob::addPartData(CDataStream&& vData)
{
    uint256 hash(Hash(vData.begin(), vData.end()));

    //maybe? mapAlreadyAskedFor.erase(CInv(MSG_PART,hash));

    auto it= mapParts.emplace(hash,CPart(hash));

    /* common part */
    CPart& part= it.first->second;
    unsigned n= vParts.size();
    vParts.push_back(&part);
    part.refs.emplace(this, n);

    /* check if the part already has data */
    if(!part.present())
    {
        /* missing data; use the supplied data */
        /* prevent calling the Complete callback FIXME: make this look better */
        cntPartsRcvd--;
        CSplitBlob::RecvPart(0, vData);
        cntPartsRcvd++;
    }
    return n;
}

CSplitBlob::~CSplitBlob()
{
    for(unsigned n= 0; n<vParts.size(); ++n)
    {
        CPart& part= *vParts[n];
        part.refs.erase(std::pair<CSplitBlob*,unsigned>(this,n));
        if(part.refs.empty())
            mapParts.erase(part.hash);
    }
}

void CSplitBlob::UseAsSource(CNode* pfrom)
{
    if(pfrom)
    {
        for ( const CPart* part : vParts )
        {
            if(!part->present())
            {
                /*Actually request the part. Inventory system will prevent redundant requests.*/
                pfrom->AskFor(CInv(MSG_PART, part->hash));
            }
        }
    }
}

bool CSplitBlob::SendPartTo(CNode* pto, const uint256& hash)
{
    auto ipart= mapParts.find(hash);

    if(ipart!=mapParts.end())
    {
        if(ipart->second.present())
        {
            pto->PushMessage("part",ipart->second.getReader());
            return true;
        }
    }
    return false;
}

// A lock needs to be taken on cs_mapManifest before calling this function.
bool CScraperManifest::AlreadyHave(CNode* pfrom, const CInv& inv)
{
    if( MSG_PART ==inv.type )
    {
        //TODO: move
        return false;
    }
    if( MSG_SCRAPERINDEX !=inv.type )
    {
        /* For any other objects, just say that we do not need it: */
        return true;
    }

    /* Inv-entory notification about scraper data index
   * see if we already have it
   * if yes, relay pfrom to Parts system as a fetch source and return true
   * else return false
  */

    auto found = mapManifest.find(inv.hash);
    if( found!=mapManifest.end() )
    {
        found->second->UseAsSource(pfrom);
        return true;
    }
    else
    {
        if(pfrom)  LogPrint("manifest", "new manifest %s from %s", inv.hash.GetHex(), pfrom->addrName);
        return false;
    }
}

// A lock needs to be taken on cs_mapManifest before calling this.
void CScraperManifest::PushInvTo(CNode* pto)
{
    /* send all keys from the index map as inventory */
    /* FIXME: advertise only completed manifests */
    for (auto const& obj : mapManifest)
    {
        pto->PushInventory(CInv(MSG_SCRAPERINDEX, obj.first));
    }
}

// A lock needs to be taken on cs_mapManifest before calling this.
bool CScraperManifest::SendManifestTo(CNode* pto, const uint256& hash)
{
    auto it = mapManifest.find(hash);

    if (it == mapManifest.end())
        return false;
    pto->PushMessage("scraperindex", *it->second);
    return true;
}


void CScraperManifest::dentry::Serialize(CDataStream& ss, int nType, int nVersion) const
{ /* TODO: remove this redundant code */
    ss<< project;
    ss<< ETag;
    ss<< LastModified;
    ss<< part1 << partc;
    ss<< GridcoinTeamID;
    ss<< current;
    ss<< last;
}
void CScraperManifest::dentry::Unserialize(CReaderStream& ss, int nType, int nVersion)
{
    ss>> project;
    ss>> ETag;
    ss>> LastModified;
    ss>> part1 >> partc;
    ss>> GridcoinTeamID;
    ss>> current;
    ss>> last;
}


void CScraperManifest::SerializeWithoutSignature(CDataStream& ss, int nType, int nVersion) const
{
    WriteCompactSize(ss, vParts.size());
    for( const CPart* part : vParts )
        ss << part->hash;
    ss<< pubkey;
    ss<< sCManifestName;
    ss<< nTime;
    ss<< ConsensusBlock;
    ss<< BeaconList << BeaconList_c;
    ss<< projects;
    ss<< nContentHash;
}

// This is to compare manifest content quickly. We just need the parts and the consensus block.
void CScraperManifest::SerializeForManifestCompare(CDataStream& ss, int nType, int nVersion) const
{
    WriteCompactSize(ss, vParts.size());
    for( const CPart* part : vParts )
        ss << part->hash;
    ss<< ConsensusBlock;
}


void CScraperManifest::Serialize(CDataStream& ss, int nType, int nVersion) const
{
    SerializeWithoutSignature(ss, nType, nVersion);
    ss << signature;
}


// This is the complement to IsScraperAuthorizedToBroadcastManifests in the scraper.
// It is used to determine whether received manifests are authorized.
bool CScraperManifest::IsManifestAuthorized(CPubKey& PubKey)
{
    bool bIsValid = PubKey.IsValid();
    if (!bIsValid)
        return false;

    CKeyID ManifestKeyID = PubKey.GetID();

    CBitcoinAddress ManifestAddress;
    ManifestAddress.Set(ManifestKeyID);

    // This is the address corresponding to the manifest public key.
    std::string sManifestAddress = ManifestAddress.ToString();

    // Now check and see if that address is in the authorized scraper list.
    AppCacheSection mScrapers = ReadCacheSection(Section::SCRAPER);

    for (auto const& entry : mScrapers)
    {
        if (entry.second.value == "true" || entry.second.value == "1")
        {
            if (sManifestAddress == entry.first)
                return true;
        }
    }

    LogPrintf("WARNING: CScraperManifest::IsManifestAuthorized: Manifest from %s is not authorized.", sManifestAddress);

    return false;
}


void CScraperManifest::UnserializeCheck(CReaderStream& ss)
{
    const auto pbegin = ss.begin();

    vector<uint256> vph;
    ss>>vph;
    ss>> pubkey;

    // This will set the bCheckAuthorized flag to false if a message
    // is received while the wallet is not in sync. If in sync and
    // the manifest is authorized, then set the checked flag to true,
    // otherwise terminate the unserializecheck and throw an error,
    // which will also result in an increase in banscore.
    if (OutOfSyncByAge())
        bCheckedAuthorized = false;
    else if (IsManifestAuthorized(pubkey))
        bCheckedAuthorized = true;
    else
        throw error("CScraperManifest::UnserializeCheck: Unapproved scraper ID");

    ss>> sCManifestName;
    ss>> nTime;
    ss>> ConsensusBlock;
    ss>> BeaconList >> BeaconList_c;
    ss>> projects;

    if(BeaconList+BeaconList_c>vph.size())
        throw error("CScraperManifest::UnserializeCheck: beacon part out of range");
    for(const dentry& prj : projects)
        if(prj.part1+prj.partc>vph.size())
            throw error("CScraperManifest::UnserializeCheck: project part out of range");

    ss >> nContentHash;

    uint256 hash = Hash(pbegin, ss.begin());

    ss >> signature;
    LogPrint("Manifest", "CScraperManifest::UnserializeCheck: hash of signature = %s", Hash(signature.begin(), signature.end()).GetHex());

    CKey mkey;
    if(!mkey.SetPubKey(pubkey))
        throw error("CScraperManifest: Invalid manifest key");
    if(!mkey.Verify(hash, signature))
        throw error("CScraperManifest: Invalid manifest signature");
    for( const uint256& ph : vph )
        addPart(ph);
}

// A lock must be taken on cs_mapManifest before calling this function.
bool CScraperManifest::DeleteManifest(const uint256& nHash)
{
    if(mapManifest.erase(nHash))
        return true;
    else
        return false;
}

// A lock needs to be taken on cs_mapManifest before calling this function.
bool CScraperManifest::RecvManifest(CNode* pfrom, CDataStream& vRecv)
{
    /* Index object for scraper data.
   * deserialize message
   * hash
   * see if we do not already have it
   * validate the message
   * populate the maps
   * request parts
  */
    /* hash the object */
    uint256 hash(Hash(vRecv.begin(), vRecv.end()));

    /* see if we do not already have it */
    if( AlreadyHave(pfrom,CInv(MSG_SCRAPERINDEX, hash)) )
    {
        return error("Already have this ScraperManifest");
    }
    const auto it = mapManifest.emplace(hash,std::unique_ptr<CScraperManifest>(new CScraperManifest()));
    CScraperManifest& manifest = *it.first->second;
    manifest.phash= &it.first->first;
    try {
        //void Unserialize(Stream& s, int nType, int nVersion)
        manifest.UnserializeCheck(vRecv);
    } catch(bool& e) {
        mapManifest.erase(hash);
        LogPrint("manifest", "invalid manifest %s received", hash.GetHex());
        if(pfrom)
        {
            LogPrintf("WARNING: CScraperManifest::RecvManifest): Invalid manifest %s received from %s. Increasing banscore by %u.",
                     hash.GetHex(), pfrom->addr.ToString(), SCRAPER_MISBEHAVING_NODE_BANSCORE);
            pfrom->Misbehaving(SCRAPER_MISBEHAVING_NODE_BANSCORE);
        }
        return false;
    } catch(std::ios_base::failure& e) {
        mapManifest.erase(hash);
        LogPrint("manifest", "invalid manifest %s received", hash.GetHex());
        if(pfrom)
        {
            LogPrintf("WARNING: CScraperManifest::RecvManifest): Invalid manifest %s received from %s. Increasing banscore by %u.",
                     hash.GetHex(), pfrom->addr.ToString(), SCRAPER_MISBEHAVING_NODE_BANSCORE);
            pfrom->Misbehaving(SCRAPER_MISBEHAVING_NODE_BANSCORE);
        }
        return false;
    }
    LogPrint("manifest", "received manifest %s with %u / %u parts", hash.GetHex(),(unsigned)manifest.cntPartsRcvd,(unsigned)manifest.vParts.size());
    if( manifest.isComplete() )
    {
        /* If we already got all the parts in memory, signal completion */
        manifest.Complete();
    } else {
        /* else request missing parts from the sender */
        manifest.UseAsSource(pfrom);
    }
    return true;
}

// A lock needs to be taken on cs_mapManifest before calling this function.
bool CScraperManifest::addManifest(std::unique_ptr<CScraperManifest>&& m, CKey& keySign)
{
    m->pubkey= keySign.GetPubKey();

    // serialize the content for comparison purposes and put in manifest.
    CDataStream sscomp(SER_NETWORK,1);
    m->SerializeForManifestCompare(sscomp, SER_NETWORK, 1);
    m->nContentHash = Hash(sscomp.begin(), sscomp.end());

    /* serialize and hash the object */
    CDataStream ss(SER_NETWORK,1);
    m->SerializeWithoutSignature(ss, SER_NETWORK, 1);
    //ss << *m;

    /* sign the serialized manifest and append the signature */
    uint256 hash(Hash(ss.begin(),ss.end()));
    keySign.Sign(hash, m->signature);
    //ss << m->signature;
    if (fDebug) LogPrintf("INFO: CScraperManifest::addManifest: hash of signature = %s", Hash(m->signature.begin(), m->signature.end()).GetHex());

    LogPrint("manifest", "adding new local manifest");
    /* at this point it is easier to pretend like it was received from network */
    // ^ Yes, but your are creating a new object and pointer that way. It is better to do
    // a special insert routine below, which forwards the object (pointer).
    // return CScraperManifest::RecvManifest(0, ss);

    /* try inserting into map */
    const auto it = mapManifest.emplace(hash, std::move(m));
    /* Already exists, do nothing */
    if (it.second == false)
        return false;

    CScraperManifest& manifest = *it.first->second;
    /* set the hash pointer inside */
    manifest.phash= &it.first->first;

    // We do not need to do a deserialize check here, because the
    // manifest originates from THIS node, and the scraper's authorization
    // to send has already been checked before the call.
    // We also do not need to do a manifest.isComplete to see if all
    // parts are available, because they have to be - this manifest was constructed
    // on THIS node.

    // Call manifest complete to notify peers of new manifest.
    manifest.Complete();
    return true;
}

void CScraperManifest::Complete()
{
    /* Notify peers that we have a new manifest */
    LogPrint("manifest", "manifest %s complete with %u parts", phash->GetHex(),(unsigned)vParts.size());
    {
        LOCK(cs_vNodes);
        for (auto const& pnode : vNodes)
            pnode->PushInventory(CInv{MSG_SCRAPERINDEX, *phash});
    }

    /* Do something with the complete manifest */
    std::string bodystr;
    vParts[0]->getReader() >> bodystr;
    if (fDebug) LogPrintf("INFO: CScraperManifest::Complete(): from %s with hash %s", sCManifestName, phash->GetHex());
}

/* how?
 * Should we only request objects that we need?
 * Because nodes should only have valid data, download anything they send.
 * They should only send what we requested, but we do not know what it is,
 * until we have it, let it pass.
 * There is 32MiB message size limit. There is a chance we could hit it, so
 * splitting is necesssary. Index object with list of parts is needed.
 *
 * If inv about index is received, and we do not know about it yet, just
 * getdata it. If it turns out useless, just ban the node. Then getdata the
 * parts from the node.
*/

UniValue CScraperManifest::ToJson() const
{
    UniValue r(UniValue::VOBJ);
#ifdef SCRAPER_NET_PK_AS_ADDRESS
    r.pushKV("pubkey",CBitcoinAddress(pubkey.GetID()).ToString());
#else
    r.pushKV("pubkey",pubkey.GetID().ToString());
#endif
    r.pushKV("sCManifestName",sCManifestName);

    r.pushKV("nTime",(int64_t)nTime);
    r.pushKV("nTime",DateTimeStrFormat(nTime));
    r.pushKV("ConsensusBlock",ConsensusBlock.GetHex());
    r.pushKV("nContentHash",nContentHash.GetHex());
    r.pushKV("BeaconList",(int64_t)BeaconList); r.pushKV("BeaconList_c",(int64_t)BeaconList_c);

    UniValue projects(UniValue::VARR);
    for( const dentry& part : this->projects )
        projects.push_back(part.ToJson());
    r.pushKV("projects",projects);

    UniValue parts(UniValue::VARR);
    for( const CPart* part : this->vParts )
        parts.push_back(part->hash.GetHex());
    r.pushKV("parts",parts);
    return r;
}
UniValue CScraperManifest::dentry::ToJson() const
{
    UniValue r(UniValue::VOBJ);
    r.pushKV("project",project);
    r.pushKV("ETag",ETag);
    r.pushKV("LastModified",DateTimeStrFormat(LastModified));
    r.pushKV("part1",(int64_t)part1); r.pushKV("partc",(int64_t)partc);
    r.pushKV("GridcoinTeamID",(int64_t)GridcoinTeamID);
    r.pushKV("current",current);
    r.pushKV("last",last);
    return r;
}

UniValue listmanifests(const UniValue& params, bool fHelp)
{
    if(fHelp || params.size() != 0 )
        throw std::runtime_error(
                "listmanifests\n"
                "Show detailed list of known ScraperManifest objects.\n"
                );
    UniValue result1(UniValue::VOBJ);

    LOCK(CScraperManifest::cs_mapManifest);

    for(const auto& pair : CScraperManifest::mapManifest)
    {
        const uint256& hash= pair.first;
        const CScraperManifest& manifest= *pair.second;
        result1.pushKV(hash.GetHex(),manifest.ToJson());
    }
    return result1;
}

UniValue getmpart(const UniValue& params, bool fHelp)
{
    if(fHelp || params.size() != 1 )
        throw std::runtime_error(
                "getmpart <hash>\n"
                "Show content of CPart object.\n"
                );
    auto ipart= CSplitBlob::mapParts.find(uint256(params[0].get_str()));
    if(ipart == CSplitBlob::mapParts.end())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Object not found");
    return UniValue(HexStr(ipart->second.data.begin(),ipart->second.data.end()));
}
