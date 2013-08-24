//
//  Paranoid Pirate queue
//
//     Andreas Hoelzlwimmer <andreas.hoelzlwimmer@fh-hagenberg.at>
//
#include "zmsg.hpp"
#include <bitcoin/bitcoin.hpp>
#include <obelisk/zmq_message.hpp>

#include <stdint.h>
#include <vector>

#define HEARTBEAT_LIVENESS  3       //  3-5 is reasonable
#define HEARTBEAT_INTERVAL  1000    //  msecs

#include "config.hpp"

//  This defines one active worker in our worker queue

typedef struct {
    std::string identity;           //  Address of worker
    int64_t     expiry;             //  Expires at this time
} worker_t;

//  Insert worker at end of queue, reset expiry
//  Worker must not already be in queue
void s_worker_append(std::vector<worker_t>& queue, std::string& identity)
{
    bool found = false;
    for (auto it = queue.begin(); it < queue.end(); ++it)
    {
        if (it->identity.compare(identity) == 0)
        {
            std::cout << "E: duplicate worker identity " << identity << std::endl;
            found = true;
            break;
        }
    }
    if (!found)
    {
        worker_t worker;
        worker.identity = identity;
        worker.expiry = s_clock() + HEARTBEAT_INTERVAL * HEARTBEAT_LIVENESS;
        queue.push_back(worker);
    }
}

//  Remove worker from queue, if present
void s_worker_delete(std::vector<worker_t>& queue, std::string& identity)
{
    for (auto it = queue.begin(); it < queue.end(); ++it)
    {
        if (it->identity.compare(identity) == 0)
        {
            it = queue.erase(it);
            break;
        }
    }
}

//  Reset worker expiry, worker must be present
void s_worker_refresh(std::vector<worker_t>& queue, std::string& identity)
{
    bool found = false;
    for (auto it = queue.begin(); it < queue.end(); ++it)
    {
        if (it->identity.compare(identity) == 0)
        {
           it->expiry = s_clock() + HEARTBEAT_INTERVAL * HEARTBEAT_LIVENESS;
           found = true;
           break;
        }
    }
    if (!found)
    {
       std::cout << "E: worker " << identity << " not ready" << std::endl;
    }
}

//  Pop next available worker off queue, return identity
std::string s_worker_dequeue(std::vector<worker_t>& queue)
{
    assert(queue.size());
    std::string identity = queue[0].identity;
    queue.erase(queue.begin());
    return identity;
}

//  Look for & kill expired workers
void s_queue_purge(std::vector<worker_t>& queue)
{
    int64_t clock = s_clock();
    for (auto it = queue.begin(); it < queue.end(); ++it)
    {
        if (clock > it->expiry)
        {
           it = queue.erase(it) - 1;
        }
    }
}

bc::data_chunk encode_uuid(const bc::data_chunk& data)
{
    static char hex_char[] = "0123456789ABCDEF";
    BITCOIN_ASSERT(data.size() == 17);
    BITCOIN_ASSERT(data[0] == 0x00);
    bc::data_chunk uuid(33);
    uuid[0] = '@';
    for (size_t i = 0; i < 16; ++i)
    {
        uuid[i * 2 + 1] = hex_char[data[i + 1] >> 4];
        uuid[i * 2 + 2] = hex_char[data[i + 1] & 15];
    }
    return uuid;
}


bc::data_chunk decode_uuid(const std::string& uuid)
{
    static char hex_to_bin[128] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* */
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9,-1,-1,-1,-1,-1,-1, /* 0..9 */
        -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* A..F */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* */
        -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* a..f */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1  /* */
    };
    BITCOIN_ASSERT(uuid.size() == 33);
    BITCOIN_ASSERT(uuid[0] == '@');
    bc::data_chunk data(17);
    data[0] = 0x00;
    for (size_t i = 0; i < 16; ++i)
        data[i + 1] =
            (hex_to_bin[uuid[i * 2 + 1] & 127] << 4) +
               (hex_to_bin[uuid[i * 2 + 2] & 127]);
    return data;
}

int main(int argc, char** argv)
{
    s_version_assert(2, 1);
    config_map_type config;
    if (argc == 2)
    {
        std::cout << "Using config file: " << argv[1] << std::endl;
        load_config(config, argv[1]);
    }
    else
        load_config(config, "balancer.cfg");

    // Prepare our context and sockets
    zmq::context_t context(1);
    zmq::socket_t frontend(context, ZMQ_ROUTER);
    zmq::socket_t backend(context, ZMQ_ROUTER);
    // For clients
    frontend.bind(config["frontend"].c_str());
    // For workers
    backend.bind(config["backend"].c_str());

    // Queue of available workers
    std::vector<worker_t> queue;
    // Send out heartbeats at regular intervals
    int64_t heartbeat_at = s_clock() + HEARTBEAT_INTERVAL;

    while (1)
    {
        zmq::pollitem_t items[] = {
            { backend,  0, ZMQ_POLLIN, 0 },
            { frontend, 0, ZMQ_POLLIN, 0 }
        };

        //  Poll frontend only if we have available workers
        if (queue.size())
            zmq::poll(items, 2, HEARTBEAT_INTERVAL * 1000);
        else
            zmq::poll(items, 1, HEARTBEAT_INTERVAL * 1000);

        // Handle worker activity on backend
        if (items [0].revents & ZMQ_POLLIN)
        {
            zmsg msg(backend);
            std::string identity(msg.unwrap());

            // Return reply to client if it's not a control message
            if (msg.parts() == 1)
            {
                if (strcmp(msg.address(), "READY") == 0)
                {
                    s_worker_delete(queue, identity);
                    s_worker_append(queue, identity);
                }
                else if (strcmp(msg.address(), "HEARTBEAT") == 0)
                {
                    s_worker_refresh(queue, identity);
                }
                else
                {
                    std::cout << "E: invalid message from " << identity << std::endl;
                    msg.dump();
                }
            }
            else
            {
                msg.send(frontend);
                s_worker_append(queue, identity);
            }
        }
        if (items [1].revents & ZMQ_POLLIN)
        {
            // Get client request.
            zmq_message msg_in;
            msg_in.recv(frontend);
            const bc::data_stack& in_parts = msg_in.parts();
            BITCOIN_ASSERT(in_parts.size() == 7);
            // First item is client's identity.
            BITCOIN_ASSERT(in_parts[0].size() == 17);
            // Second item is worker identity or nothing.
            BITCOIN_ASSERT(in_parts[1].size() == 17 || in_parts[1].empty());

            // We now deconstruct the request message to the frontend
            // which looks like:
            //   [CLIENT UUID]
            //   [WORKER UUID]
            //   ...
            // And create a new message that looks like:
            //   [WORKER UUID]
            //   [CLIENT UUID]
            //   ...
            // Before sending it to the worker.

            zmq_message msg_out;
            // If second frame is nothing, then we select a random worker.
            if (in_parts[1].empty())
            {
                // Route to next worker
                std::string worker_identity = s_worker_dequeue(queue);
                msg_out.append(decode_uuid(worker_identity));
            }
            else
            {
                // Route to client's preferred worker.
                msg_out.append(in_parts[1]);
            }
            msg_out.append(in_parts[0]);
            // Copy the remaining data.
            for (auto it = in_parts.begin() + 2; it != in_parts.end(); ++it)
                msg_out.append(*it);
            msg_out.send(backend);
        }

        //  Send heartbeats to idle workers if it's time
        if (s_clock() > heartbeat_at)
        {
            for (auto it = queue.begin(); it < queue.end(); ++it)
            {
                zmsg msg("HEARTBEAT");
                msg.wrap(it->identity.c_str(), NULL);
                msg.send(backend);
            }
            heartbeat_at = s_clock() + HEARTBEAT_INTERVAL;
        }
        s_queue_purge(queue);
    }
    //  We never exit the main loop
    //  But pretend to do the right shutdown anyhow
    queue.clear();
    return 0;
}
