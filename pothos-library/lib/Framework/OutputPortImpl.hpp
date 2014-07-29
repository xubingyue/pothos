// Copyright (c) 2014-2014 Josh Blum
// SPDX-License-Identifier: BSL-1.0

#pragma once
#include "Framework/PortSubscriber.hpp"
#include <Pothos/Framework/OutputPort.hpp>
#include <Pothos/Framework/BufferManager.hpp>
#include <Pothos/Util/RingDeque.hpp>
#include <vector>

class Pothos::OutputPortImpl
{
public:
    OutputPortImpl(void):
        actor(nullptr),
        isSignal(false),
        readBeforeWritePort(nullptr),
        _bufferFromManager(false){}
    BufferManager::Sptr bufferManager;
    std::vector<Label> postedLabels;
    Util::RingDeque<BufferChunk> postedBuffers;
    std::vector<PortSubscriber> subscribers;
    WorkerActor *actor;
    bool isSignal;
    InputPort *readBeforeWritePort;
    bool _bufferFromManager;
};