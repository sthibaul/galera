/*
 * Copyright (C) 2009 Codership Oy <info@codership.com>
 */

#include "gmcast.hpp"
#include "gmcast_proto.hpp"

#include "gcomm/conf.hpp"
#include "gcomm/util.hpp"
#include "gcomm/map.hpp"
#include "defaults.hpp"
#include "gu_convert.hpp"
#include "gu_resolver.hpp"

using namespace std;
using namespace std::rel_ops;
using namespace gcomm;
using namespace gcomm::gmcast;
using namespace gu;
using namespace gu::net;
using namespace gu::datetime;

static void set_tcp_defaults (URI* uri)
{
    // what happens if there is already this parameter?
    uri->set_query_param(Conf::TcpNonBlocking, gu::to_string(1));
}


static bool check_uri(const URI& uri)
{
    return (uri.get_scheme() == Conf::TcpScheme);
}



GMCast::GMCast(Protonet& net_, const string& uri_)
    :
    Transport     (net_, uri_),
    my_uuid       (0, 0),
    group_name    (),
    listen_addr   (Conf::TcpScheme + "://0.0.0.0"), // how to make it IPv6 safe?
    initial_addr  (""),
    listener      (0),
    pending_addrs (),
    remote_addrs  (),
    proto_map     (new ProtoMap()),
    check_period  ("PT1S"),
    next_check    (Date::now())
{
    if (uri.get_scheme() != Conf::GMCastScheme)
    {
        gu_throw_error (EINVAL) << "Invalid URL scheme: "
                                << uri.get_scheme();
    }
    
    // @todo: technically group name should be in path component
    try
    {
        group_name = uri.get_option (Conf::GMCastGroup);
    }
    catch (gu::NotFound&)
    {
        gu_throw_error (EINVAL) << "Group not defined in URL: "
                                << uri.to_string();
    }
    
    try
    {
        if (!host_is_any(uri.get_host()))
        {
            string port;
            try
            {
                port = uri.get_port();
            } 
            catch (gu::NotSet& )
            {
                port = Defaults::GMCastTcpPort;
            }
            initial_addr = resolve(
                Conf::TcpScheme + "://" + uri.get_host() + ":" + port).to_string();
            if (check_uri(initial_addr) == false)
            {
                gu_throw_error (EINVAL) << "initial addr '" << initial_addr
                                        << "' is not valid";
            }
            log_debug << self_string() << " initial addr: " << initial_addr;
        }
    }
    catch (gu::NotSet&)
    {
        //@note: this is different from empty host and indicates URL without ://
        gu_throw_error (EINVAL) << "Host not defined in URL: "
                                << uri.to_string();
    }
    
    try
    {
        listen_addr = uri.get_option (Conf::GMCastListenAddr);
    }
    catch (gu::NotFound&) {}
    
    try
    { 
        gu::URI(listen_addr).get_port();
    }
    catch (gu::NotSet&)
    {
        // if no port is set for listen address in the options,
        // try one from authority part
        try
        {
            listen_addr += ':' + uri.get_port();
        }
        catch (gu::NotSet&)
        {
            listen_addr += ':' + Defaults::GMCastTcpPort;
        }
    }
    
    listen_addr = resolve(listen_addr).to_string();
    if (check_uri(listen_addr) == false)
    {
        gu_throw_error (EINVAL) << "listen addr '" << listen_addr
                                << "' is not valid";
    }

    log_debug << self_string() << " listening " << listen_addr;
}

GMCast::~GMCast()
{
    if (listener != 0) close();
    
    delete proto_map;
}

void GMCast::connect() 
{    
    pstack.push_proto(this);
    log_debug << "gmcast " << get_uuid() << " connect";
    URI listen_uri(listen_addr);
    
    set_tcp_defaults (&listen_uri);

    listener = Transport::create(get_pnet(), listen_uri.to_string());
    gu_trace (listener->listen());
    
    if (initial_addr != "")
    {
        insert_address(initial_addr, UUID(), pending_addrs);
        gu_trace (gmcast_connect(initial_addr));
    }
}


void GMCast::close() 
{
    log_debug << "gmcast " << get_uuid() << " close";
    pstack.pop_proto(this);
    gcomm_assert(listener != 0);
    listener->close();
    delete listener;
    listener = 0;    
    
    for (ProtoMap::iterator i = proto_map->begin(); i != proto_map->end(); ++i)
    {
        delete ProtoMap::get_value(i);
    }

    proto_map->clear();
    pending_addrs.clear();
    remote_addrs.clear();
}


void GMCast::gmcast_accept() 
{
    Transport* tp(0);
    
    try
    {
        tp = listener->accept();
    }
    catch (Exception& e)
    {
        log_warn << e.what();
        return;
    }
    
    Proto* peer = new Proto (tp, listen_addr, "", get_uuid(), group_name);
    pair<ProtoMap::iterator, bool> ret =     
        proto_map->insert(make_pair(tp->get_fd(), peer));
    
    if (ret.second == false)
    {
        delete peer;
        gu_throw_fatal << "Failed to add peer to map";
    }
    
    peer->send_handshake();
}


void GMCast::gmcast_connect(const string& remote_addr)
{
    if (remote_addr == listen_addr) return;

    URI connect_uri(remote_addr);
    
    set_tcp_defaults (&connect_uri);
    
    Transport* tp = Transport::create(get_pnet(), connect_uri.to_string());
    
    try 
    {
        tp->connect();
    }
    catch (Exception& e)
    {
        log_debug << "Connect failed: " << e.what();
        delete tp;
        return;
    }
    
    Proto* peer = new Proto (tp, listen_addr, remote_addr,
                             get_uuid(), group_name);
    
    pair<ProtoMap::iterator, bool> ret = 
        proto_map->insert(make_pair(tp->get_fd(), peer));
    
    if (ret.second == false)
    {
        delete peer;
        gu_throw_fatal << "Failed to add peer to map";
    }
    
    ret.first->second->wait_handshake();
}


void GMCast::gmcast_forget(const UUID& uuid)
{
    /* Close all proto entries corresponding to uuid */
    
    ProtoMap::iterator pi, pi_next;
    for (pi = proto_map->begin(); pi != proto_map->end(); pi = pi_next)
    {
        pi_next = pi, ++pi_next;
        Proto* rp = ProtoMap::get_value(pi);
        if (rp->get_remote_uuid() == uuid)
        {
            delete rp;
            proto_map->erase(pi);
        }
    }
    
    /* Set all corresponding entries in address list to have retry cnt 
     * max_retry_cnt + 1 and next reconnect time after some period */
    AddrList::iterator ai;
    for (ai = remote_addrs.begin(); ai != remote_addrs.end(); ++ai)
    {
        AddrEntry& ae(AddrList::get_value(ai));
        if (ae.get_uuid() == uuid)
        {
            ae.set_retry_cnt(max_retry_cnt + 1);
            ae.set_next_reconnect(Date::now() + Period("PT5S"));
        }
    }
    
    /* Update state */
    update_addresses();
}

void GMCast::handle_connected(Proto* rp)
{
    const Transport* tp = rp->get_transport();
    assert(tp->get_state() == Transport::S_CONNECTED);
    log_debug << "transport " << tp->get_fd() << " connected";
}

void GMCast::handle_established(Proto* est)
{
    log_debug << self_string() << " connection established to "
              << est->get_remote_uuid() << " "
              << est->get_remote_addr();
    
    // If address is found from pending_addrs, move it to remote_addrs list
    // and set retry cnt to -1
    const string& remote_addr = est->get_remote_addr();
    AddrList::iterator i = pending_addrs.find (remote_addr);
    
    if (i != pending_addrs.end())
    {
        log_debug << "Erasing " << remote_addr << " from panding list";
        
        pending_addrs.erase(i);
    }
    
    if ((i = remote_addrs.find(remote_addr)) == remote_addrs.end())
    {
        log_debug << "Inserting " << remote_addr << " to remote list";
        
        insert_address (remote_addr, est->get_remote_uuid(), remote_addrs);
        i = remote_addrs.find(remote_addr);
    }
    
    AddrList::get_value(i).set_retry_cnt(max_retry_cnt - 60);
    
    // Cleanup all previously established entries with same 
    // remote uuid. It is assumed that the most recent connection
    // is usually the healthiest one.
    ProtoMap::iterator j, j_next;
    for (j = proto_map->begin(); j != proto_map->end(); j = j_next)
    {
        j_next = j, ++j_next;
        Proto* p(ProtoMap::get_value(j));
        if (p->get_remote_uuid() == est->get_remote_uuid())
        {
            if (p->get_handshake_uuid() < est->get_handshake_uuid())
            {
                log_debug << self_string()
                          << " cleaning up duplicate "
                          << p->get_transport()->get_fd() 
                          << " after established " 
                          << est->get_transport()->get_fd();
                proto_map->erase(j);
                delete p;
            }
            else if (p->get_handshake_uuid() > est->get_handshake_uuid())
            {
                log_debug << self_string()
                          << " cleaning up established "
                          << est->get_transport()->get_fd() 
                          << " which is duplicate of " 
                          << p->get_transport()->get_fd();
                proto_map->erase(proto_map->find_checked(est->get_transport()->get_fd()));
                delete est;
                break;
            }
            else
            {
                assert(p == est);
            }
        }
    }
    
    update_addresses();
}

void GMCast::handle_failed(Proto* failed)
{
    const string& remote_addr = failed->get_remote_addr();
    
    bool found_ok(false);
    for (ProtoMap::const_iterator i = proto_map->begin(); 
         i != proto_map->end(); ++i)
    {
        Proto* p(ProtoMap::get_value(i));
        if (p->get_state()       <= Proto::S_OK && 
            p->get_remote_uuid() == failed->get_remote_uuid())
        {
            found_ok = true;
            break;
        }
    }
    
    if (found_ok == false && remote_addr != "")
    {
        AddrList::iterator i;
        
        if ((i = pending_addrs.find(remote_addr)) != pending_addrs.end() ||
            (i = remote_addrs.find(remote_addr))  != remote_addrs.end())
        {
            AddrEntry& ae(AddrList::get_value(i));
            ae.set_retry_cnt(ae.get_retry_cnt() + 1);

            Date rtime = Date::now() + Period("PT1S");
            log_debug << self_string()
                      << " setting next reconnect time to "
                      << rtime << " for " << remote_addr;
            ae.set_next_reconnect(rtime);
        }
    }
    
    proto_map->erase(failed->get_transport()->get_fd());
    delete failed;
    update_addresses();
}


bool GMCast::is_connected(const string& addr, const UUID& uuid) const
{
    for (ProtoMap::const_iterator i = proto_map->begin();
         i != proto_map->end(); ++i)
    {
        Proto* conn = ProtoMap::get_value(i);
        
        if (addr == conn->get_remote_addr() || 
            uuid == conn->get_remote_uuid())
        {
            return true;
        }
    }
    
    return false;
}


void GMCast::insert_address (const string& addr,
                             const UUID&   uuid,
                             AddrList&     alist)
{
    if (addr == listen_addr)
    {
        gu_throw_fatal << "Trying to add self to addr list";
    }
    
    if (alist.insert(make_pair(addr, 
                               AddrEntry(Date::now(),
                                         Date::now(), uuid))).second == false)
    {
        log_warn << "Duplicate entry: " << addr;
    }
    else
    {
        log_debug << self_string() << ": new address entry " << uuid << ' '
                  << addr;
    }
}


void GMCast::update_addresses()
{
    LinkMap link_map;
    set<UUID> uuids;
    /* Add all established connections into uuid_map and update 
     * list of remote addresses */

    ProtoMap::iterator i, i_next;
    for (i = proto_map->begin(); i != proto_map->end(); i = i_next)
    {
        i_next = i, ++i_next;
        Proto* rp = ProtoMap::get_value(i);
        
        if (rp->get_state() == Proto::S_OK)
        {
            if (rp->get_remote_addr() == "" || 
                rp->get_remote_uuid() == UUID::nil())
            {
                gu_throw_fatal << "Protocol error: local: (" << my_uuid
                               << ", '" << listen_addr
                               << "'), remote: (" << rp->get_remote_uuid()
                               << ", '" << rp->get_remote_addr() << "')";
            }
            
            if (remote_addrs.find(rp->get_remote_addr()) == remote_addrs.end())
            {
                log_warn << "Connection exists but no addr on addr list for "
                         << rp->get_remote_addr();
                insert_address(rp->get_remote_addr(), rp->get_remote_uuid(), 
                               remote_addrs);
            }

            if (uuids.insert(rp->get_remote_uuid()).second == false)
            {
                // Duplicate entry, drop this one
                // @todo Deeper inspection about the connection states
                log_debug << self_string() << " dropping duplicate entry";
                proto_map->erase(i);
                delete rp;
            }
            else
            {
                link_map.insert(Link(rp->get_remote_uuid(),
                                     rp->get_remote_addr()));
            }
        }
    }
    
    /* Send topology change message containing only established 
     * connections */
    for (ProtoMap::iterator i = proto_map->begin(); i != proto_map->end(); ++i)
    {
        Proto* gp = ProtoMap::get_value(i);
        
        // @todo: a lot of stuff here is done for each connection, including
        //        message creation and serialization. Need a mcast_msg() call
        //        and move this loop in there.
        if (gp->get_state() == Proto::S_OK)
            gp->send_topology_change(link_map);
    }
    
    /* Add entries reported by all other nodes to address list to 
     * get complete view of existing uuids/addresses */
    for (ProtoMap::iterator i = proto_map->begin(); i != proto_map->end(); ++i)
    {
        Proto* rp = ProtoMap::get_value(i);
        
        if (rp->get_state() == Proto::S_OK)
        {
            for (LinkMap::const_iterator j = rp->get_link_map().begin();
                 j != rp->get_link_map().end(); ++j)
            {
                const UUID& link_uuid(LinkMap::get_key(j));
                const string& link_addr(LinkMap::get_value(j).get_addr());
                gcomm_assert(link_uuid != UUID::nil() && link_addr != "");
                
                if (link_uuid                     != get_uuid()         &&
                    remote_addrs.find(link_addr)  == remote_addrs.end() &&
                    pending_addrs.find(link_addr) == pending_addrs.end())
                {
                    log_debug << self_string() 
                              << " conn refers to but no addr in addr list for "
                              << link_addr;
                    insert_address(link_addr, link_uuid, pending_addrs);
                    AddrList::iterator pi(pending_addrs.find(link_addr));
                    assert(pi != pending_addrs.end());
                    AddrEntry& ae(AddrList::get_value(pi));
                    // Try to connect 60 times before forgetting
                    ae.set_retry_cnt(max_retry_cnt - 60);
                    // Add some randomness for first reconnect to avoid
                    // simultaneous connects
                    Date rtime(Date::now());
                    rtime = rtime + ::rand() % (100*MSec);
                    ae.set_next_reconnect(rtime);
                    next_check = min(next_check, rtime);
                }
            }
        }
    }
}


void GMCast::reconnect()
{
    /* Loop over known remote addresses and connect if proto entry 
     * does not exist */
    Date now = Date::now();
    AddrList::iterator i, i_next;
    
    for (i = pending_addrs.begin(); i != pending_addrs.end(); i = i_next)
    {
        i_next = i, ++i_next;

        const string& pending_addr(AddrList::get_key(i));
        const AddrEntry& ae(AddrList::get_value(i));
        
        if (is_connected (pending_addr, UUID::nil()) == false)
        {
            if (ae.get_retry_cnt() > max_retry_cnt)
            {
                log_debug << "Forgetting " << pending_addr;
                pending_addrs.erase(i);
                continue; // no reference to pending_addr after this
            }
            else if (ae.get_next_reconnect() <= now)
            {
                // log_debug << "Connecting to " << pending_addr;
                gmcast_connect (pending_addr);
            }
        }
    }
    
    for (i = remote_addrs.begin(); i != remote_addrs.end(); i = i_next) 
    {
        i_next = i, ++i_next;
        
        const string& remote_addr(AddrList::get_key(i));
        const AddrEntry& ae(AddrList::get_value(i));
        const UUID& remote_uuid(ae.get_uuid());
        
        gcomm_assert(remote_uuid != get_uuid());
        
        if (is_connected (remote_addr, remote_uuid) == false)
        {
            if (ae.get_retry_cnt() > max_retry_cnt)
            {
                log_debug << " Forgetting " << remote_uuid << " ("
                          << remote_addr << ")";
                remote_addrs.erase(i);
                continue;//no reference to remote_addr or remote_uuid after this
            }
            else if (ae.get_next_reconnect() <= now)
            {
                if (ae.get_retry_cnt() % 30 == 0)
                {
                    log_info << self_string() << " reconnecting to " 
                             << remote_uuid << " (" << remote_addr
                             << "), attempt " << ae.get_retry_cnt();
                }
                
                gmcast_connect(remote_addr);
            }
            else
            {
                // 
            }
        }
    }
}


Date gcomm::GMCast::handle_timers()
{
    const Date now(Date::now());
    if (now >= next_check)
    {
        reconnect();
        next_check = now + check_period;
    }
    return next_check;
}




void GMCast::handle_up(int id, const Datagram& dg, const ProtoUpMeta& um) 
{
    ProtoMap::iterator i;

    if (listener == 0)
    {
        return;
    }
    
    if (id == listener->get_fd())
    {
        gmcast_accept();
    }
    else if ((i = proto_map->find(id)) != proto_map->end())
    {
        Proto* p(ProtoMap::get_value(i));
        if (dg.get_len() > 0)
        {
            const Proto::State prev_state(p->get_state());
            if (prev_state == Proto::S_FAILED)
            {
                log_warn << "unhandled failed proto";
                handle_failed(p);
                return;
            }
            Message msg;
            msg.unserialize(&dg.get_payload()[0], dg.get_len(), dg.get_offset());
            if (msg.get_type() >= Message::T_USER_BASE)
            {
                send_up(Datagram(dg, dg.get_offset() + msg.serial_size()),
                        ProtoUpMeta(msg.get_source_uuid()));
            }
            else
            {
                gu_trace(p->handle_message(msg));
                if (p->get_changed() == true)
                {
                    update_addresses();
                    reconnect();
                }
            }
            if (prev_state     != Proto::S_OK && 
                p->get_state() == Proto::S_OK)
            {
                handle_established(p);
            }
        }
        else if (p->get_transport()->get_state() == Transport::S_CONNECTED &&
                 (p->get_state() == Proto::S_HANDSHAKE_WAIT ||
                  p->get_state() == Proto::S_INIT))
        {
            handle_connected(p);
        }
        else if (p->get_transport()->get_state() == Transport::S_CONNECTED)
        {
            log_warn << "zero len datagram";
        } 
        else
        {
            p->set_state(Proto::S_FAILED);
            handle_failed(p);
        }
    }
    else
    {
        // log_debug << "proto entry not found";
    }
}

int GMCast::handle_down(const Datagram& dg, const ProtoDownMeta& dm) 
{
    Message msg(Message::T_USER_BASE, get_uuid(), 1);
    Datagram my_dg(dg);
    gu_trace(push_header(msg, my_dg));
    
    for (ProtoMap::iterator i = proto_map->begin(); i != proto_map->end(); ++i)
    {
        Proto* rp = ProtoMap::get_value(i);
        int err;
        if ((err = rp->get_transport()->handle_down(my_dg, 
                                                    ProtoDownMeta())) != 0)
        {
            log_debug << "transport: " << ::strerror(err);
        }
    }
    
    return 0;
}

void gcomm::GMCast::handle_stable_view(const View& view)
{
    log_info << "GMCast::handle_stable_view: " << view;
    if (view.get_type() == V_PRIM)
    {
        std::set<UUID> gmcast_lst;
        for (AddrList::const_iterator i(remote_addrs.begin());
             i != remote_addrs.end(); ++i)
        {
            gmcast_lst.insert(i->second.get_uuid());
        }
        std::set<UUID> view_lst;
        for (NodeList::const_iterator i(view.get_members().begin());
             i != view.get_members().end(); ++i)
        {
            view_lst.insert(i->first);
        }
        std::list<UUID> diff;
        std::set_difference(gmcast_lst.begin(),
                            gmcast_lst.end(),
                            view_lst.begin(),
                            view_lst.end(),
                            std::back_inserter(diff));

        for (std::list<UUID>::const_iterator i(diff.begin());
             i != diff.end(); ++i)
        {
            gmcast_forget(*i);
        }

        for (std::set<UUID>::const_iterator i(view_lst.begin());
             i != view_lst.end(); ++i)
        {
            AddrList::iterator ai;
            if ((ai = find_if(remote_addrs.begin(), remote_addrs.end(),
                              AddrListUUIDCmp(*i))) != remote_addrs.end())
            {
                log_info << "declaring " << *i << " stable";
                ai->second.set_retry_cnt(-1);
            }
        }
    }
}

