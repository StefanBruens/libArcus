/*
 * This file is part of libArcus
 *
 * Copyright (C) 2015 Ultimaker b.v. <a.hiemstra@ultimaker.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License v3.0 as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License v3.0 for more details.
 * You should have received a copy of the GNU Lesser General Public License v3.0
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Socket.h"
#include "Socket_p.h"

#include <algorithm>

using namespace Arcus;

Socket::Socket() : d(new Private)
{
}

Socket::~Socket()
{
    {
        std::lock_guard<std::mutex> lk(d->receiveQueueMutexBlock);
        d->message_received_condition_variable = true;
    }
    d->socket_block_condition_variable.notify_all();

    if(d->thread)
    {
        if(d->state != SocketState::Closed || d->state != SocketState::Error)
        {
            close();
        }
        delete d->thread;
    }

    for(SocketListener* listener : d->listeners)
    {
        /* If deleting the socket listener while another thread is reporting an
         * error due to closing the socket, deleting the listener causes another
         * error to report and this causes an infinite loop. Making sure the
         * listener is dysfunctional before deleting it prevents this. */
        listener->_socket = nullptr;

        delete listener;
    }
}

SocketState::SocketState Socket::getState() const
{
    return d->state;
}

Error Socket::getLastError() const
{
    return d->last_error;
}

void Socket::clearError()
{
    d->last_error = Error();
}

bool Socket::registerMessageType(const google::protobuf::Message* message_type)
{
    if(d->state != SocketState::Initial)
    {
        d->error(ErrorCode::InvalidStateError, "Socket is not in initial state");
        return false;
    }

    return d->message_types.registerMessageType(message_type);
}

bool Socket::registerAllMessageTypes(const std::string& file_name)
{
    if(file_name.empty())
    {
        d->error(ErrorCode::MessageRegistrationFailedError, "Empty file name");
        return false;
    }

    if(d->state != SocketState::Initial)
    {
        d->error(ErrorCode::MessageRegistrationFailedError, "Socket is not in initial state");
        return false;
    }

    if(!d->message_types.registerAllMessageTypes(file_name))
    {
        d->error(ErrorCode::MessageRegistrationFailedError, d->message_types.getErrorMessages());
        return false;
    }

    return true;
}

void Socket::addListener(SocketListener* listener)
{
    if(d->state != SocketState::Initial)
    {
        d->error(ErrorCode::InvalidStateError, "Socket is not in initial state");
        return;
    }

    listener->setSocket(this);
    d->listeners.push_back(listener);
}

void Socket::removeListener(SocketListener* listener)
{
    if(d->state != SocketState::Initial)
    {
        d->error(ErrorCode::InvalidStateError, "Socket is not in initial state");
        return;
    }

    auto itr = std::find(d->listeners.begin(), d->listeners.end(), listener);
    d->listeners.erase(itr);
}

void Socket::connect(const std::string& address, int port)
{
    if(d->state != SocketState::Initial || d->thread != nullptr)
    {
        d->error(ErrorCode::InvalidStateError, "Socket is not in initial state");
        return;
    }

    d->address = address;
    d->port = port;
    d->thread = new std::thread([&]() { d->run(); });
    d->next_state = SocketState::Connecting;
}

void Socket::reset()
{
    if (d->state != SocketState::Closed && d->state != SocketState::Error)
    {
        d->error(ErrorCode::InvalidStateError, "Socket is not in closed or error state");
        return;
    }

    if(d->thread)
    {
        d->thread->join();
        d->thread = nullptr;
    }

    d->state = SocketState::Initial;
    d->next_state = SocketState::Initial;
    clearError();
}

void Socket::listen(const std::string& address, int port)
{
    if(d->state != SocketState::Initial || d->thread != nullptr)
    {
        d->error(ErrorCode::InvalidStateError, "Socket is not in initial state");
        return;
    }
    
    d->address = address;
    d->port = port;
    d->thread = new std::thread([&]() { d->run(); });
    d->next_state = SocketState::Opening;
}

void Socket::close()
{
    {
        std::lock_guard<std::mutex> lk(d->receiveQueueMutexBlock);
        d->message_received_condition_variable = true;
    }
    d->socket_block_condition_variable.notify_all();

    if(d->state == SocketState::Initial)
    {
        d->error(ErrorCode::InvalidStateError, "Cannot close a socket in initial state");
        return;
    }

    if(d->state == SocketState::Closed || d->state == SocketState::Error)
    {
        // Silently ignore this, as calling close on an already closed socket should be fine.
        return;
    }

    if(d->state == SocketState::Connected)
    {
        // Make the socket request close.
        d->next_state = SocketState::Closing;

        // Wait with closing until we properly clear the send queue.
        while(d->state == SocketState::Closing)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    else
    {
        // We are still in an unconnected state but want to abort any connection
        // attempt. So disable any communication on the socket to make sure calls
        // like accept() exit, then close the socket.
        d->platform_socket.shutdown(PlatformSocket::ShutdownDirection::ShutdownBoth);
        d->platform_socket.close();
        d->next_state = SocketState::Closed;
    }

    if(d->thread)
    {
        d->thread->join();
        delete d->thread;
        d->thread = nullptr;
    }
}

void Socket::sendMessage(MessagePtr message)
{
    if(!message)
    {
        d->error(ErrorCode::InvalidMessageError, "Message cannot be nullptr");
        return;
    }

    std::lock_guard<std::mutex> lock(d->sendQueueMutex);
    d->sendQueue.push_back(message);
}

MessagePtr Socket::takeNextMessage()
{
    // Set 'listener' in wait mode until a new message received
    std::unique_lock<std::mutex> lk(d->receiveQueueMutexBlock);

    while (d->message_received_condition_variable == false)
    {
        d->socket_block_condition_variable.wait(lk);
    }

    std::lock_guard<std::mutex> lock(d->receiveQueueMutex);
    if(d->receiveQueue.size() > 0)
    {
        MessagePtr next = d->receiveQueue.front();
        d->receiveQueue.pop_front();
        return next;
    }
    else
    {
        // For the next request the listener will have 'wait' mode,
        // The wait mode will be released only afeter receiving a new message
        d->message_received_condition_variable = false;
        return nullptr;
    }
}

MessagePtr Arcus::Socket::createMessage(const std::string& type)
{
    return d->message_types.createMessage(type);
}
