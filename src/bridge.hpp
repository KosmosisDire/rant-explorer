/* The web build's node: the part of rant::Node the capture uses, under the same names, run
   through the bridge client in the page. Values stay Rant's wire bytes, on one thread. */
#ifndef BRIDGE_HPP
#define BRIDGE_HPP

#include <rant.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bridge {

class Node;

/* A bridge or mesh event as one line. */
class Event {
public:
    explicit Event(std::string text) : text_(std::move(text)) {}
    const std::string& to_string() const { return text_; }

private:
    std::string text_;
};

/* One value as a handler sees it: its bytes, the schema they were written in, when it came. */
class MessageView {
public:
    MessageView(rant::Bytes data, const rant::detail::RantSchema* schema, uint64_t recv_us)
        : data_(data), schema_(schema), recv_us_(recv_us) {}
    rant::Bytes                     data() const { return data_; }
    rant::Bytes                     value() const { return data_; }
    const rant::detail::RantSchema* raw_schema() const { return schema_; }
    uint64_t                        recv_us() const { return recv_us_; }

private:
    rant::Bytes                     data_;
    const rant::detail::RantSchema* schema_;
    uint64_t                        recv_us_;
};
using VariableUpdate = MessageView;

template <class T> class ResponseView;
template <> class ResponseView<rant::Bytes> {
public:
    ResponseView(rant::CallStatus status, std::string message, MessageView value)
        : status_(status), message_(std::move(message)), value_(value) {}
    rant::CallStatus                status() const { return status_; }
    std::string_view                message() const { return message_; }
    rant::Bytes                     data() const { return value_.data(); }
    const rant::detail::RantSchema* raw_schema() const { return value_.raw_schema(); }

private:
    rant::CallStatus status_;
    std::string      message_;
    MessageView      value_;
};

template <class T> class ProgressView;
template <> class ProgressView<rant::Bytes> {
public:
    explicit ProgressView(MessageView value) : value_(value) {}
    bool                            has_value() const { return value_.data().size() > 0; }
    rant::Bytes                     data() const { return value_.data(); }
    const rant::detail::RantSchema* raw_schema() const { return value_.raw_schema(); }
    uint64_t                        recv_us() const { return value_.recv_us(); }

private:
    MessageView value_;
};

using MessageHandler  = std::function<void(const MessageView&)>;
using ResponseHandler = std::function<void(const ResponseView<rant::Bytes>&)>;
using ProgressHandler = std::function<void(const ProgressView<rant::Bytes>&)>;

/* A share of one entity on the bridge. A copy names the same entity, and destroying one
   closes nothing, as with Rant's handles. */
class Handle {
public:
    Handle() = default;
    bool             valid() const { return id_ != 0; }
    int              match_count() const;
    rant::SendStatus close();
    /* Takes the mesh's types again, for a handle typed from it. */
    bool             refresh();

protected:
    explicit Handle(int id) : id_(id) {}
    int id_ = 0;
    friend class Node;
};

template <class T> class Subscriber : public Handle {
public:
    Subscriber() = default;
    explicit Subscriber(int id) : Handle(id) {}
};

template <class T> class Publisher : public Handle {
public:
    Publisher() = default;
    explicit Publisher(int id) : Handle(id) {}
    rant::SendStatus send(rant::Bytes value);
};

template <class T> class RemoteVariable : public Handle {
public:
    RemoteVariable() = default;
    explicit RemoteVariable(int id) : Handle(id) {}
    /* Fires with the current value once there is one, then on every change. */
    void             on_change(MessageHandler handler);
    rant::SendStatus set(rant::Bytes value);
};

template <class Q, class P> class RemoteFunction : public Handle {
public:
    RemoteFunction() = default;
    explicit RemoteFunction(int id) : Handle(id) {}
    rant::SendStatus call_async(rant::Bytes request, ResponseHandler done);
};

template <class Q, class G, class P> class RemoteTask : public Handle {
public:
    RemoteTask() = default;
    explicit RemoteTask(int id) : Handle(id) {}
    rant::TaskCall   call_async(rant::Bytes request, ProgressHandler progress, ResponseHandler done);
    rant::SendStatus cancel(uint32_t call);
};

/* The mesh as the bridge last reported it. */
class Reflection {
public:
    std::vector<rant::Peer>   peers() const;
    /* Moves whenever the entities below may have. */
    uint32_t                  epoch() const;
    std::vector<rant::Entity> entities(uint32_t peer) const;
    std::vector<rant::Entity> mesh() const;
    rant::SendStatus meta_async(uint32_t peer, std::function<void(const rant::MetaSnapshot&)> done,
                                uint32_t sections) const;
};

class Node {
public:
    /* Binds to the node the page opened. Its name and options are the page's. */
    Node(const std::string& name, const rant::NodeOptions& opts);

    bool        valid() const;
    std::string last_error() const;
    void        on_event(std::function<void(const Event&)> handler);
    bool        on_log(std::function<void(const rant::LogLine&)> handler);
    Reflection  reflection() const { return Reflection(); }
    rant::Schema schema_from_wire(rant::Bytes wire) const;

    template <class T>
    Subscriber<T> subscriber(const std::string& name, MessageHandler handler, const rant::Qos& qos,
                             const rant::Schema& schema = {})
    {
        return Subscriber<T>(open_topic("subscriber", name, qos, schema, std::move(handler)));
    }
    template <class T>
    Publisher<T> publisher(const std::string& name, const rant::Qos& qos, const rant::Schema& schema = {})
    {
        return Publisher<T>(open_topic("publisher", name, qos, schema, nullptr));
    }
    template <class T>
    RemoteVariable<T> remote_variable(const std::string& name, const rant::VariableOptions<T>& opts)
    {
        return RemoteVariable<T>(open_variable(name, opts.read_only, opts.reflect_from_mesh));
    }
    template <class Q, class P>
    RemoteFunction<Q, P> remote_function(const std::string& name, const rant::FunctionOptions&)
    {
        return RemoteFunction<Q, P>(open_call("remote_function", name));
    }
    template <class Q, class G, class P>
    RemoteTask<Q, G, P> remote_task(const std::string& name, const rant::TaskOptions&)
    {
        return RemoteTask<Q, G, P>(open_call("remote_task", name));
    }

private:
    static int open_topic(const char* kind, const std::string& name, const rant::Qos& qos,
                          const rant::Schema& schema, MessageHandler handler);
    static int open_variable(const std::string& name, bool read_only, bool reflect);
    static int open_call(const char* kind, const std::string& name);
};

/* The writes every typed handle shares, by handle id. */
rant::SendStatus send(int id, rant::Bytes value);
void             listen(int id, MessageHandler handler);
rant::SendStatus call(int id, rant::Bytes request, ProgressHandler progress, ResponseHandler done, uint32_t* call_id);
rant::SendStatus cancel(int id, uint32_t call);

template <class T> rant::SendStatus Publisher<T>::send(rant::Bytes value) { return bridge::send(id_, value); }
template <class T> void RemoteVariable<T>::on_change(MessageHandler handler) { listen(id_, std::move(handler)); }
template <class T> rant::SendStatus RemoteVariable<T>::set(rant::Bytes value) { return bridge::send(id_, value); }

template <class Q, class P>
rant::SendStatus RemoteFunction<Q, P>::call_async(rant::Bytes request, ResponseHandler done)
{
    return call(id_, request, nullptr, std::move(done), nullptr);
}

template <class Q, class G, class P>
rant::TaskCall RemoteTask<Q, G, P>::call_async(rant::Bytes request, ProgressHandler progress, ResponseHandler done)
{
    rant::TaskCall out;
    out.status = call(id_, request, std::move(progress), std::move(done), &out.id);
    return out;
}

template <class Q, class G, class P>
rant::SendStatus RemoteTask<Q, G, P>::cancel(uint32_t call_id) { return bridge::cancel(id_, call_id); }

} /* namespace bridge */

#endif /* BRIDGE_HPP */
