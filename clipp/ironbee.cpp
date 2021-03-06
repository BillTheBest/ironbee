/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ****************************************************************************/

/**
 * @file
 * @brief IronBee &mdash; CLIPP IronBee Consumer Implementation
 *
 * @author Christopher Alfeld <calfeld@qualys.com>
 */

#include "ironbee.hpp"
#include "control.hpp"

#include <ironbeepp/all.hpp>
#include <ironbee/action.h>
#include <ironbee/debug.h>

#include <boost/make_shared.hpp>
#include <boost/thread.hpp>

using namespace std;

using boost::make_shared;

namespace IronBee {
namespace CLIPP {

namespace {

class adapt_header :
    public unary_function<const Input::header_t, IronBee::ParsedNameValue>
{
public:
    explicit adapt_header(IronBee::MemoryPool mp) :
        m_mp(mp)
    {
        // nop
    }

    IronBee::ParsedNameValue operator()(const Input::header_t& header) const
    {
        return IronBee::ParsedNameValue::create(
            m_mp,
            IronBee::ByteString::create_alias(
                m_mp, header.first.data, header.first.length
            ),
            IronBee::ByteString::create_alias(
                m_mp, header.second.data, header.second.length
            )
        );
    }

private:
    IronBee::MemoryPool m_mp;
};

class IronBeeDelegate :
    public Input::Delegate
{
public:
    explicit
    IronBeeDelegate(IronBee::Engine engine) :
        m_engine(engine)
    {
        // nop
    }

    ~IronBeeDelegate()
    {
        if (m_connection) {
            boost::lock_guard<boost::mutex> guard(m_mutex);
            m_connection.destroy();
        }
    }

    void connection_opened(const Input::ConnectionEvent& event)
    {
        using namespace boost;

        {
            lock_guard<mutex> guard(m_mutex);

            if (m_connection) {
                m_connection.destroy();
            }
            m_connection = IronBee::Connection::create(m_engine);

            char* local_ip = strndup(
                event.local_ip.data,
                event.local_ip.length
            );
            m_connection.memory_pool().register_cleanup(bind(free,local_ip));
            char* remote_ip = strndup(
                event.remote_ip.data,
                event.remote_ip.length
            );
            m_connection.memory_pool().register_cleanup(bind(free,remote_ip));

            m_connection.set_local_ip_string(local_ip);
            m_connection.set_local_port(event.local_port);
            m_connection.set_remote_ip_string(remote_ip);
            m_connection.set_remote_port(event.remote_port);
        }

        m_engine.notify().connection_opened(m_connection);
    }

    void connection_closed(const Input::NullEvent& event)
    {
        if (! m_connection) {
            throw runtime_error(
                "CONNECTION_CLOSED event fired outside "
                "of connection lifetime."
            );
        }
        m_engine.notify().connection_closed(m_connection);

        {
            boost::lock_guard<boost::mutex> guard(m_mutex);
            m_connection.destroy();
        }
        m_connection = IronBee::Connection();
    };

    void connection_data_in(const Input::DataEvent& event)
    {
        if (! m_connection) {
            throw runtime_error(
                "CONNECTION_DATA_IN event fired outside "
                "of connection lifetime."
            );
        }

        // Copy because IronBee needs mutable input.
        IronBee::ConnectionData data = IronBee::ConnectionData::create(
            m_connection,
            event.data.data,
            event.data.length
        );

        m_engine.notify().connection_data_in(data);
    }

    void connection_data_out(const Input::DataEvent& event)
    {
        if (! m_connection) {
            throw runtime_error(
                "CONNECTION_DATA_IN event fired outside "
                "of connection lifetime."
            );
        }

        // Copy because IronBee needs mutable input.
        IronBee::ConnectionData data = IronBee::ConnectionData::create(
            m_connection,
            event.data.data,
            event.data.length
        );

        m_engine.notify().connection_data_out(data);
    }

    void request_started(const Input::RequestEvent& event)
    {
        if (! m_connection) {
            throw runtime_error(
                "REQUEST_STARTED event fired outside "
                "of connection lifetime."
            );
        }

        if (m_transaction) {
            m_transaction.destroy();
        }
        m_transaction = IronBee::Transaction::create(m_connection);

        IronBee::ParsedRequestLine prl =
            IronBee::ParsedRequestLine::create_alias(
                m_transaction,
                event.raw.data,      event.raw.length,
                event.method.data,   event.method.length,
                event.uri.data,      event.uri.length,
                event.protocol.data, event.protocol.length
            );

        m_engine.notify().request_started(m_transaction, prl);
    }

    void request_header(const Input::HeaderEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "REQUEST_HEADER_FINISHED event fired outside "
                "of connection lifetime."
            );
        }

        adapt_header adaptor(m_transaction.memory_pool());
        m_engine.notify().request_header_data(
            m_transaction,
            boost::make_transform_iterator(event.headers.begin(), adaptor),
            boost::make_transform_iterator(event.headers.end(),   adaptor)
        );
    }

    void request_header_finished(const Input::NullEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "REQUEST_HEADER_FINISHED event fired outside "
                "of connection lifetime."
            );
        }
        m_engine.notify().request_header_finished(m_transaction);
    }

    void request_body(const Input::DataEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "REQUEST_BODY event fired outside "
                "of connection lifetime."
            );
        }

        // Copy because IronBee needs mutable input.
        char* mutable_data = strndup(event.data.data, event.data.length);
        m_connection.memory_pool().register_cleanup(
            boost::bind(free, mutable_data)
        );
        IronBee::TransactionData data =
            IronBee::TransactionData::create_alias(
                m_connection.memory_pool(),
                mutable_data,
                event.data.length
            );

        m_engine.notify().request_body_data(m_transaction, data);
    }

    void request_finished(const Input::NullEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "REQUEST_FINISHED event fired outside "
                "of transaction lifetime."
            );
        }
        m_engine.notify().request_finished(m_transaction);
    }

    void response_started(const Input::ResponseEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "RESPONSE_STARTED event fired outside "
                "of transaction lifetime."
            );
        }

        IronBee::ParsedResponseLine prl =
            IronBee::ParsedResponseLine::create_alias(
                m_transaction,
                event.raw.data,      event.raw.length,
                event.protocol.data, event.protocol.length,
                event.status.data,   event.status.length,
                event.message.data,  event.message.length
            );

        m_engine.notify().response_started(m_transaction, prl);
    }

    void response_header(const Input::HeaderEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "RESPONSE_HEADER event fired outside "
                "of connection lifetime."
            );
        }

        adapt_header adaptor(m_transaction.memory_pool());
        m_engine.notify().response_header_data(
            m_transaction,
            boost::make_transform_iterator(event.headers.begin(), adaptor),
            boost::make_transform_iterator(event.headers.end(),   adaptor)
        );
    }

    void response_header_finished(const Input::NullEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "RESPONSE_HEADER_FINISHED event fired outside "
                "of connection lifetime."
            );
        }
        m_engine.notify().response_header_finished(m_transaction);
    }

    void response_body(const Input::DataEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "RESPONSE_BODY event fired outside "
                "of connection lifetime."
            );
        }

        // Copy because IronBee needs mutable input.
        char* mutable_data = strndup(event.data.data, event.data.length);
        m_connection.memory_pool().register_cleanup(
            boost::bind(free, mutable_data)
        );
        IronBee::TransactionData data =
            IronBee::TransactionData::create_alias(
                m_connection.memory_pool(),
                mutable_data,
                event.data.length
            );

        m_engine.notify().response_body_data(m_transaction, data);
    }

    void response_finished(const Input::NullEvent& event)
    {
        if (! m_transaction) {
            throw runtime_error(
                "RESPONSE_FINISHED event fired outside "
                "of connection lifetime."
            );
        }

        m_engine.notify().response_finished(m_transaction);
        m_transaction.destroy();
        m_transaction = IronBee::Transaction();
    }

private:
    IronBee::Engine      m_engine;
    IronBee::Connection  m_connection;
    IronBee::Transaction m_transaction;
    boost::mutex         m_mutex;
};

void load_configuration(IronBee::Engine engine, const std::string& path)
{
    engine.notify().configuration_started();

    IronBee::ConfigurationParser parser
        = IronBee::ConfigurationParser::create(engine);

    parser.parse_file(path);

    parser.destroy();
    engine.notify().configuration_finished();
}

enum action_e {
    ACTION_ALLOW,
    ACTION_BLOCK,
    ACTION_BREAK
};

// We need symbols to have the instance data point to as C++ doesn't allow
// casting integers to void*s.
static const action_e c_allow = ACTION_ALLOW;
static const action_e c_block = ACTION_BLOCK;
static const action_e c_break = ACTION_BREAK;

extern "C" {

ib_status_t clipp_action_create(
    ib_engine_t*      ib,
    ib_context_t*     ctx,
    ib_mpool_t*       mp,
    const char*       params,
    ib_action_inst_t* inst,
    void*             cbdata
)
{
    IB_FTRACE_INIT();

    static const string allow_arg("allow");
    static const string block_arg("block");
    static const string break_arg("break");

    // const_cast is necessary because of C APIs lack of type safety.
    if (params == allow_arg) {
        inst->data = const_cast<action_e*>(&c_allow);
    }
    else if (params == block_arg) {
        inst->data = const_cast<action_e*>(&c_block);
    }
    else if (params == break_arg) {
        inst->data = const_cast<action_e*>(&c_break);
    }
    else {
        ib_log_error(ib, "Unknown argument for clipp: %s", params);
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

ib_status_t clipp_action_execute(
    void*      data,
    ib_rule_t* rule,
    ib_tx_t*   tx,
    ib_flags_t flags,
    void*      cbdata
)
{
    IB_FTRACE_INIT();

    action_e action      = *reinterpret_cast<action_e*>(data);
    action_e* out_action = reinterpret_cast<action_e*>(cbdata);

    *out_action = action;

    IB_FTRACE_RET_STATUS(IB_OK);
}

} // extern "C"

// Move to new file?
template <typename WorkType>
class FunctionWorkerPool :
    private boost::noncopyable
{
    typedef boost::unique_lock<boost::mutex> lock_t;

    void do_work()
    {
        WorkType local_work;
        for (;;) {
            {
                lock_t lock(m_mutex);
                ++m_num_workers_available;

            }
            m_worker_available_cv.notify_one();

            {
                lock_t lock(m_mutex);
                while (! m_work_available) {
                    m_work_available_cv.wait(lock);
                    if (m_shutdown) {
                        return;
                    }
                }

                local_work = m_work;
                m_work_available = false;
                --m_num_workers_available;
            }
            m_work_accepted_barrier.wait();

            m_work_function(local_work);
        }
    }

public:
    FunctionWorkerPool(
        size_t                          num_workers,
        boost::function<void(WorkType)> work_function
    ) :
        m_num_workers(num_workers),
        m_work_function(work_function),
        m_work_accepted_barrier(2),
        m_num_workers_available(0),
        m_work_available(false),
        m_shutdown(false)
    {
        for (size_t i = 0; i < num_workers; ++i) {
            m_thread_group.create_thread(boost::bind(
                &FunctionWorkerPool::do_work,
                this
            ));
        }
    }

    void operator()(WorkType work)
    {
        {
            lock_t lock(m_mutex);

            while (m_num_workers_available == 0) {
                m_worker_available_cv.wait(lock);
            }

            m_work           = work;
            m_work_available = true;

            m_work_available_cv.notify_one();
        }
        m_work_accepted_barrier.wait();
    }

    void shutdown()
    {
        {
            lock_t lock(m_mutex);
            while (m_num_workers_available < m_num_workers) {
                m_worker_available_cv.wait(lock);
            }
        }

        m_shutdown = true;
        m_work_available_cv.notify_all();

        m_thread_group.join_all();
    }

private:
    size_t                          m_num_workers;
    boost::function<void(WorkType)> m_work_function;

    boost::mutex                    m_mutex;
    boost::condition_variable       m_worker_available_cv;
    boost::condition_variable       m_work_available_cv;
    boost::barrier                  m_work_accepted_barrier;
    boost::thread_group             m_thread_group;
    size_t                          m_num_workers_available;
    bool                            m_work_available;
    bool                            m_shutdown;
    WorkType                        m_work;
};

} // Anonymous

struct IronBeeConsumer::State
{
    boost::function<bool(Input::input_p&)> modifier;
};

IronBeeConsumer::IronBeeConsumer(const string& config_path) :
    m_state(make_shared<State>())
{
    m_state->modifier = IronBeeModifier(config_path);
}

bool IronBeeConsumer::operator()(const Input::input_p& input)
{
    // Insist the input not modified.
    Input::input_p copy = input;
    m_state->modifier(copy);

    return true;
}

struct IronBeeModifier::State
{
    State() :
        server_value(__FILE__, "clipp")
    {
        IronBee::initialize();
        engine = IronBee::Engine::create(server_value.get());
    }

    ~State()
    {
        engine.destroy();
        IronBee::shutdown();
    }

    behavior_e           behavior;
    action_e             current_action;
    IronBee::Engine      engine;
    IronBee::ServerValue server_value;
};

IronBeeModifier::IronBeeModifier(
    const string& config_path,
    behavior_e    behavior
) :
    m_state(make_shared<State>())
{
    m_state->behavior = behavior;

    ib_status_t rc = ib_action_register(
        m_state->engine.ib(),
        "clipp",
        IB_ACT_FLAG_NONE,
        clipp_action_create,
        NULL,
        NULL,
        NULL,
        clipp_action_execute,
        reinterpret_cast<void*>(&m_state->current_action)
    );
    if (rc != IB_OK) {
        throw runtime_error("Could not register clipp action.");
    }

    load_configuration(m_state->engine, config_path);
}

bool IronBeeModifier::operator()(Input::input_p& input)
{
    if (! input) {
        return true;
    }

    IronBeeDelegate delegate(m_state->engine);

    switch (m_state->behavior) {
        case ALLOW: m_state->current_action = ACTION_ALLOW;  break;
        case BLOCK: m_state->current_action = ACTION_BLOCK; break;
        default:
            throw logic_error("Unknown behavior.  Please report as bug.");
    }

    input->connection.dispatch(delegate, true);

    switch (m_state->current_action) {
        case ACTION_ALLOW: return true;
        case ACTION_BLOCK: return false;
        case ACTION_BREAK: throw clipp_break();
        default:
            throw logic_error("Unknown clipp action.  Please report as bug.");
    }
}

struct IronBeeThreadedConsumer::State
{
    void process_input(Input::input_p input)
    {
        if (! input) {
            return;
        }

        IronBeeDelegate delegate(engine);
        input->connection.dispatch(delegate, true);
    }

    explicit
    State(size_t num_workers) :
        worker_pool(
            num_workers,
            boost::bind(
                &IronBeeThreadedConsumer::State::process_input,
                this,
                _1
            )
        ),
        server_value(__FILE__, "clipp")

    {
        IronBee::initialize();
        engine = IronBee::Engine::create(server_value.get());
    }

    ~State()
    {
        worker_pool.shutdown();

        engine.destroy();
        IronBee::shutdown();
    }


    FunctionWorkerPool<Input::input_p> worker_pool;
    IronBee::Engine      engine;
    IronBee::ServerValue server_value;
};

IronBeeThreadedConsumer::IronBeeThreadedConsumer(
    const string& config_path,
    size_t        num_workers
) :
    m_state(make_shared<State>(num_workers))
{
    load_configuration(m_state->engine, config_path);
}

bool IronBeeThreadedConsumer::operator()(const Input::input_p& input)
{
    m_state->worker_pool(input);

    return true;
}

} // CLIPP
} // IronBee
