// Copyright (c) 2014-2014 Josh Blum
// SPDX-License-Identifier: BSL-1.0

#include "Framework/WorkerActor.hpp"
#include <Poco/Format.h>
#include <Poco/Logger.h>
#include <algorithm> //sort
#include <cassert>

void Pothos::WorkerActor::handleAsyncPortMessage(const PortMessage<InputPort *, Object> &message, const Theron::Address)
{
    assert(message.id != nullptr);
    auto &input = *message.id;
    if (input._impl->asyncMessages.full()) input._impl->asyncMessages.set_capacity(input._impl->asyncMessages.capacity()*2);
    if (input._impl->isSlot and message.contents.type() == typeid(ObjectVector))
    {
        POTHOS_EXCEPTION_TRY
        {
            const auto &args = message.contents.extract<ObjectVector>();
            block->opaqueCallHandler(input.name(), args.data(), args.size());
        }
        POTHOS_EXCEPTION_CATCH(const Exception &ex)
        {
            poco_error_f2(Poco::Logger::get("Pothos.Block.callSlot"), "%s: %s", block->getName(), ex.displayText());
        }
        this->bump();
        return;
    }
    input._impl->asyncMessages.push_back(message.contents);
    this->notify();
}

void Pothos::WorkerActor::handleLabelsPortMessage(const PortMessage<InputPort *, LabeledBuffersMessage> &message, const Theron::Address)
{
    assert(message.id != nullptr);
    auto &input = *message.id;

    //insert labels (in order) and adjust for the current offset
    for (const auto &byteOffsetLabel : message.contents.labels)
    {
        auto label = byteOffsetLabel;
        auto elemSize = input.dtype().size();
        label.index += input.totalElements()*elemSize; //increment by absolute byte count
        label.index += input._impl->bufferAccumulator.getTotalBytesAvailable(); //increment by enqueued bytes
        label.index /= elemSize; //convert from bytes to elements
        input._impl->inlineMessages.push_back(label);
    }

    //push all buffers into the accumulator
    for (const auto &buffer : message.contents.buffers)
    {
        input._impl->bufferAccumulator.push(buffer);
    }

    this->notify();
}

void Pothos::WorkerActor::handleBufferPortMessage(const PortMessage<InputPort *, BufferChunk> &message, const Theron::Address)
{
    assert(message.id != nullptr);
    auto &input = *message.id;
    input._impl->bufferAccumulator.push(message.contents);
    this->notify();
}

void Pothos::WorkerActor::handleBufferReturnMessage(const BufferReturnMessage &message, const Theron::Address)
{
    auto mgr = message.buff.getBufferManager();
    if (mgr)
    {
        mgr->push(message.buff);
        this->notify();
    }
    else this->bump();
}

void Pothos::WorkerActor::handleSubscriberPortMessage(const PortMessage<std::string, PortSubscriberMessage> &message, const Theron::Address from)
{
    try
    {
        //extract the list of subscribers
        std::vector<PortSubscriber> *subscribers = nullptr;
        if (message.contents.action.find("INPUT") != std::string::npos)
        {
            assert(message.contents.port.inputPort != nullptr);
            auto &port = getOutput(message.id, __FUNCTION__);
            subscribers = &port._impl->subscribers;
        }
        if (message.contents.action.find("OUTPUT") != std::string::npos)
        {
            assert(message.contents.port.outputPort != nullptr);
            auto &port = getInput(message.id, __FUNCTION__);
            subscribers = &port._impl->subscribers;
        }
        assert(subscribers != nullptr);

        //locate the subscriber in the list
        auto sub = message.contents.port;
        auto it = std::find(subscribers->begin(), subscribers->end(), sub);
        const bool found = it != subscribers->end();

        //subscriber is an input, add to the outputs subscribers list
        if (message.contents.action == "SUBINPUT")
        {
            if (found) throw PortAccessError("Pothos::WorkerActor::handleSubscriberPortMessage("+message.contents.action+")",
                Poco::format("input %s subscription exsists in output port %s", message.contents.port.inputPort->name(), message.id));
            subscribers->push_back(sub);
        }

        //subscriber is an output, add to the input subscribers list
        if (message.contents.action == "SUBOUTPUT")
        {
            if (found) throw PortAccessError("Pothos::WorkerActor::handleSubscriberPortMessage("+message.contents.action+")",
                Poco::format("output %s subscription exsists in input port %s", message.contents.port.outputPort->name(), message.id));
            subscribers->push_back(sub);
        }

        //unsubscriber is an input, remove from the outputs subscribers list
        if (message.contents.action == "UNSUBINPUT")
        {
            if (not found) throw PortAccessError("Pothos::WorkerActor::handleSubscriberPortMessage("+message.contents.action+")",
                Poco::format("input %s subscription missing from output port %s", message.contents.port.inputPort->name(), message.id));
            subscribers->erase(it);
        }

        //unsubscriber is an output, remove from the inputs subscribers list
        if (message.contents.action == "UNSUBOUTPUT")
        {
            if (not found) throw PortAccessError("Pothos::WorkerActor::handleSubscriberPortMessage("+message.contents.action+")",
                Poco::format("output %s subscription missing from input port %s", message.contents.port.outputPort->name(), message.id));
            subscribers->erase(it);
        }

        this->updatePorts();

        if (from != Theron::Address::Null()) this->Send(std::string(""), from);
    }
    catch (const Pothos::Exception &ex)
    {
        if (from != Theron::Address::Null()) this->Send(ex.displayText(), from);
    }
    this->bump();
}

void Pothos::WorkerActor::handleBumpWorkMessage(const BumpWorkMessage &, const Theron::Address)
{
    this->notify();
}

static void bufferManagerPushExternal(
    std::shared_ptr<Theron::Framework> framework,
    const Theron::Address &addr,
    const Pothos::ManagedBuffer &buff
)
{
    BufferReturnMessage message;
    message.buff = buff;
    framework->Send(message, Theron::Address::Null(), addr);
}

void Pothos::WorkerActor::handleActivateWorkMessage(const ActivateWorkMessage &, const Theron::Address from)
{
    //setup the buffer return callback on the manager
    for (auto &entry : this->outputs)
    {
        auto &port = *entry.second;
        auto &mgr = port._impl->bufferManager;
        if (mgr) mgr->setCallback(std::bind(&bufferManagerPushExternal,
            std::static_pointer_cast<Theron::Framework>(block->_threadPool.getContainer()), this->GetAddress(), std::placeholders::_1));
    }

    POTHOS_EXCEPTION_TRY
    {
        this->block->activate();
        this->activeState = true;
        this->Send(std::string(""), from);
    }
    POTHOS_EXCEPTION_CATCH(const Exception &ex)
    {
        this->Send(ex.displayText(), from);
    }
    this->bump();
}

void Pothos::WorkerActor::handleDeactivateWorkMessage(const DeactivateWorkMessage &, const Theron::Address from)
{
    POTHOS_EXCEPTION_TRY
    {
        this->activeState = false;
        this->block->deactivate();
        this->Send(std::string(""), from);
    }
    POTHOS_EXCEPTION_CATCH(const Exception &ex)
    {
        this->Send(ex.displayText(), from);
    }
}

void Pothos::WorkerActor::handleShutdownActorMessage(const ShutdownActorMessage &message, const Theron::Address from)
{
    this->outputs.clear();
    this->inputs.clear();
    this->updatePorts();

    if (from != Theron::Address::Null()) this->Send(message, from);
}

void Pothos::WorkerActor::handleRequestPortInfoMessage(const RequestPortInfoMessage &message, const Theron::Address from)
{
    if (message.isInput) this->Send(getInput(message.name, __FUNCTION__).dtype(), from);
    else                 this->Send(getOutput(message.name, __FUNCTION__).dtype(), from);
    this->bump();
}

void Pothos::WorkerActor::handleRequestWorkerStatsMessage(const RequestWorkerStatsMessage &, const Theron::Address from)
{
    this->workStats.ticksStatsQuery = Theron::Detail::Clock::GetTicks();
    this->Send(this->workStats, from);
    this->bump();
}

void Pothos::WorkerActor::handleOpaqueCallMessage(const OpaqueCallMessage &message, const Theron::Address from)
{
    OpaqueCallResultMessage result;
    POTHOS_EXCEPTION_TRY
    {
        result.obj = this->block->opaqueCallHandler(message.name, message.inputArgs, message.numArgs);
    }
    POTHOS_EXCEPTION_CATCH(const Pothos::Exception &ex)
    {
        result.error.reset(ex.clone());
    }
    this->Send(result, from);
    this->bump();
}