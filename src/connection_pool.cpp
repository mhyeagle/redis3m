// Copyright (c) 2014 Luca Marturana. All rights reserved.
// Licensed under Apache 2.0, see LICENSE for details

#include <redis3m/connection_pool.h>
#include <redis3m/command.h>
#include <redis3m/utils/resolv.h>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>
#include <redis3m/utils/logging.h>
#include <boost/thread.hpp>
#include <boost/algorithm/string/find.hpp>
#include <boost/regex.hpp>

using namespace redis3m;

connection_pool::connection_pool(const std::string& sentinel_host,
                                 const std::string& master_name,
                                 unsigned int sentinel_port):
master_name(master_name),
sentinel_port(sentinel_port),
_database(0)
{
    boost::algorithm::split(sentinel_hosts, sentinel_host, boost::is_any_of(","), boost::token_compress_on);
}

connection::ptr_t connection_pool::get(connection::role_t type)
{
    connection::ptr_t ret;

    // Look for a cached connection
    access_mutex.lock();
    std::set<connection::ptr_t>::const_iterator it;
    switch (type) {
        case connection::ANY:
            it = connections.begin();
            break;
        case connection::MASTER:
        case connection::SLAVE:
            it = std::find_if(connections.begin(), connections.end(),
                           ( boost::lambda::bind(&connection::_role, *boost::lambda::_1) == type ));
            break;
    }
    if (it != connections.end())
    {
        ret = *it;
        connections.erase(it);
    }
    access_mutex.unlock();

    // If no connection found, create a new one
    if (!ret)
    {
        switch (type) {
            case connection::SLAVE:
            {
                ret = create_slave_connection();
                break;
            }
            case connection::ANY:
            {
                try {
                    ret = create_slave_connection();
                    break;
                } catch (const cannot_find_slave& ex) {
                    // Go ahead, looking for a master, no break istruction
                    logging::debug("Slave not found, looking for a master");
                }
            }
            case connection::MASTER:
                ret = create_master_connection();
                break;
        }

        // Setup connections selecting db
        if (_database != 0)
        {
            reply r = ret->run(command("SELECT")(boost::lexical_cast<std::string>(_database)));
            if (r.type() == reply::ERROR)
            {
                throw wrong_database(r.str());
            }
        }
    }
    return ret;
}

void connection_pool::put(connection::ptr_t conn)
{
    if (conn->is_valid())
    {
        boost::unique_lock<boost::mutex> lock(access_mutex);
        connections.insert(conn);
    }
}

connection::ptr_t connection_pool::sentinel_connection()
{
    BOOST_FOREACH(const std::string& host, sentinel_hosts)
    {
        std::vector<std::string> real_sentinels = resolv::get_addresses(host);
        logging::debug(boost::str(
                           boost::format("Found %d redis sentinels: %s")
                           % real_sentinels.size()
                           % boost::algorithm::join(real_sentinels, ", ")
                           )
                       );
        BOOST_FOREACH( const std::string& real_sentinel, real_sentinels)
        {
            logging::debug(boost::str(boost::format("Trying sentinel %s") % real_sentinel));
            try
            {
                return connection::create(real_sentinel, sentinel_port);
            } catch (const unable_to_connect& ex)
            {
                logging::debug(boost::str(boost::format("%s is down") % real_sentinel));
            }
        }
    }
    throw cannot_find_sentinel("Cannot find sentinel");
}

connection::role_t connection_pool::get_role(connection::ptr_t conn)
{
    static const boost::regex role_searcher("\r\nrole:([a-z]+)\r\n");

    reply r = conn->run(command("ROLE"));
    std::string role_s;

    if (r.type() == reply::ERROR && boost::algorithm::find_first(r.str(), "unknown"))
    {
        logging::debug("Old redis, doesn't support ROLE command");
        reply r = conn->run(command("INFO") << "replication");
        boost::smatch results;
        if (boost::regex_search(r.str(), results, role_searcher))
        {
            role_s = results[1];
        }
    }
    else if (r.type() == reply::ARRAY)
    {
        role_s = r.elements().at(0).str();
    }

    if (role_s == "master")
    {
        return connection::MASTER;
    }
    else if (role_s == "slave")
    {
        return connection::SLAVE;
    }
    else
    {
        return connection::ANY;
    }
}

connection::ptr_t connection_pool::create_slave_connection()
{
    connection::ptr_t sentinel = sentinel_connection();
    sentinel->append(command("SENTINEL") << "slaves" << master_name );
    reply response = sentinel->get_reply();
    std::vector<reply> slaves(response.elements());
    std::random_shuffle(slaves.begin(), slaves.end());

    for (std::vector<reply>::const_iterator it = slaves.begin();
         it != slaves.end(); ++it)
    {
        const std::vector<reply>& properties = it->elements();
        if (properties.at(9).str() == "slave")
        {
            std::string host = properties.at(3).str();
            unsigned int port = boost::lexical_cast<unsigned int>(properties.at(5).str());
            try
            {
                connection::ptr_t conn = connection::create(host, port);
                connection::role_t role = get_role(conn);
                if (role == connection::SLAVE)
                {
                    conn->_role = role;
                    return conn;
                }
                else
                {
                    logging::debug(boost::str(boost::format("Error on connection to %s:%d declared to be slave but it's not, waiting") % host % port));
                }
            } catch (const unable_to_connect& ex)
            {
                logging::debug(boost::str(boost::format("Error on connection to Slave %s:%d declared to be up") % host % port));
            }
        }
    }
    throw cannot_find_slave();
}

connection::ptr_t connection_pool::create_master_connection()
{
    unsigned int connection_retries = 0;
    while(connection_retries < 20)
    {
        connection::ptr_t sentinel = sentinel_connection();
        reply masters = sentinel->run(command("SENTINEL") << "masters" );
        BOOST_FOREACH(const reply& master, masters.elements())
        {
            if (master.elements().at(1).str() == master_name)
            {
                const std::string& flags = master.elements().at(9).str();
                if (flags == "master")
                {
                    const std::string& master_ip = master.elements().at(3).str();
                    unsigned int master_port = boost::lexical_cast<unsigned int>(master.elements().at(5).str());

                    try
                    {
                        connection::ptr_t conn = connection::create(master_ip, master_port);
                        connection::role_t role = get_role(conn);
                        if (role == connection::MASTER)
                        {
                            conn->_role = role;
                            return conn;
                        }
                        else
                        {
                            logging::debug(boost::str(boost::format("Error on connection to %s:%d declared to be master but it's not, waiting") % master_ip % master_port));
                        }
                    } catch (const unable_to_connect& ex)
                    {
                        logging::debug(boost::str(boost::format("Error on connection to Master %s:%d declared to be up, waiting") % master_ip % master_port));
                    }
                }
            }
        }
        connection_retries++;
#if BOOST_VERSION < 105500
        boost::this_thread::sleep(boost::posix_time::seconds(5));
#else
        boost::this_thread::sleep_for(boost::chrono::seconds(5));
#endif
    }
    throw cannot_find_master(boost::str(boost::format("Unable to find master of name: %s (too much retries") % master_name));
}

template<>
void connection_pool::run_with_connection(boost::function<void(connection::ptr_t)> f,
                                connection::role_t conn_type,
                                unsigned int retries)
{
    while (retries > 0)
    {
        try
        {
            connection::ptr_t c = get(conn_type);
            f(c);
            put(c);
            return;
        }
        catch (const connection_error& ex)
        {
            --retries;
        }
    }
    throw too_much_retries();
}
