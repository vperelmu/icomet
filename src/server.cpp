#include "../config.h"
#include <http-internal.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>
#include "server.h"
#include "server_config.h"
#include "util/log.h"
#include "util/list.h"

class HttpQuery{
private:
	struct evkeyvalq params;
public:
	HttpQuery(struct evhttp_request *req){
		evhttp_parse_query(evhttp_request_get_uri(req), &params);
	}
	int get_int(const char *name, int def){
		const char *val = evhttp_find_header(&params, name);
		return val? atoi(val) : def;
	}
	const char* get_str(const char *name, const char *def){
		const char *val = evhttp_find_header(&params, name);
		return val? val : def;
	}
};

Server::Server(){
	this->auth = AUTH_NONE;
	subscribers = 0;
	sub_pool.pre_alloc(1024);

	channel_slots.resize(ServerConfig::max_channels);
	for(int i=0; i<channel_slots.size(); i++){
		Channel *channel = &channel_slots[i];
		channel->id = i;
		free_channels.push_back(channel);
	}
}

Server::~Server(){
}

Channel* Server::get_channel(int cid){
	if(cid < 0 || cid >= channel_slots.size()){
		return NULL;
	}
	return &channel_slots[cid];
}

Channel* Server::get_channel_by_name(const std::string &cname){
	std::map<std::string, Channel *>::iterator it;
	it = cname_channels.find(cname);
	if(it == cname_channels.end()){
		return NULL;
	}
	return it->second;
}

Channel* Server::alloc_channel(Channel *channel){
	if(channel == NULL){
		channel = free_channels.head;
	}
	assert(channel->subs.size == 0);
	log_debug("alloc channel: %d", channel->id);
	// first remove, then push_back, do not mistake the order
	free_channels.remove(channel);
	used_channels.push_back(channel);
	cname_channels[channel->name] = channel;
	return channel;
}

void Server::delete_channel(Channel *channel){
	assert(channel->subs.size == 0);
	log_debug("delete channel: %d", channel->id);
	// first remove, then push_back, do not mistake the order
	used_channels.remove(channel);
	free_channels.push_back(channel);
	cname_channels.erase(channel->name);
	channel->reset();
}

static void on_connection_close(struct evhttp_connection *evcon, void *arg){
	log_trace("connection closed");
	Subscriber *sub = (Subscriber *)arg;
	Server *serv = sub->serv;
	serv->sub_end(sub);
}

int Server::check_timeout(){
	//log_debug("<");
	struct evbuffer *buf = evbuffer_new();
	Channel *channel_next = NULL;
	for(Channel *channel = used_channels.head; channel; channel=channel_next){
		channel_next = channel->next;
		
		if(channel->subs.size == 0){
			if(--channel->idle < 0){
				this->delete_channel(channel);
			}
			continue;
		}
		if(channel->idle < ServerConfig::channel_idles){
			channel->idle = ServerConfig::channel_idles;
		}

		Subscriber *sub_next = NULL;
		for(Subscriber *sub = channel->subs.head; sub; sub=sub_next){
			sub_next = sub->next;
			
			if(++sub->idle <= ServerConfig::polling_idles){
				continue;
			}
			evbuffer_add_printf(buf,
				"%s({type: \"noop\", cid: \"%d\", seq: \"%d\"});\n",
				sub->callback.c_str(),
				channel->id,
				sub->noop_seq
				);
			evhttp_send_reply_chunk(sub->req, buf);
			evhttp_send_reply_end(sub->req);
			//
			evhttp_connection_set_closecb(sub->req->evcon, NULL, NULL);
			this->sub_end(sub);
		}
	}
	evbuffer_free(buf);
	//log_debug(">");
	return 0;
}

int Server::sub_end(Subscriber *sub){
	Channel *channel = sub->channel;
	channel->del_subscriber(sub);
	subscribers --;
	log_debug("%s:%d sub_end %d, channels: %d, subs: %d",
		sub->req->remote_host, sub->req->remote_port,
		channel->id, used_channels.size, channel->subs.size);
	sub_pool.free(sub);
	return 0;
}

int Server::sub(struct evhttp_request *req){
	if(evhttp_request_get_command(req) != EVHTTP_REQ_GET){
		evhttp_send_reply(req, 405, "Method Not Allowed", NULL);
		return 0;
	}
	bufferevent_enable(evhttp_connection_get_bufferevent(req->evcon), EV_READ);

	HttpQuery query(req);
	int cid = query.get_int("cid", -1);
	int seq = query.get_int("seq", 0);
	int noop = query.get_int("noop", 0);
	const char *cb = query.get_str("cb", DEFAULT_JSONP_CALLBACK);
	const char *token = query.get_str("token", "");
	
	Channel *channel = this->get_channel(cid);
	if(!channel){
		struct evbuffer *buf = evbuffer_new();
		evbuffer_add_printf(buf,
			"%s({type: \"404\", cid: \"%d\", seq: \"0\", content: \"Not Found\"});\n",
			cb,
			cid);
		evhttp_send_reply(req, HTTP_OK, "OK", buf);
		evbuffer_free(buf);
		return 0;
	}
	if(this->auth == AUTH_TOKEN &&
		(channel->idle == -1 || channel->token.empty() || channel->token != token))
	{
		log_debug("%s:%d, Token Error, cid: %d, token: %s",
			req->remote_host,
			req->remote_port,
			cid,
			token
			);
		struct evbuffer *buf = evbuffer_new();
		evbuffer_add_printf(buf,
			"%s({type: \"401\", cid: \"%d\", seq: \"0\", content: \"Token Error\"});\n",
			cb,
			cid);
		evhttp_send_reply(req, HTTP_OK, "OK", buf);
		evbuffer_free(buf);
		return 0;
	}
	if(channel->subs.size >= ServerConfig::max_subscribers_per_channel){
		log_debug("%s:%d, Too Many Requests, cid: %d",
			req->remote_host,
			req->remote_port,
			cid
			);
		struct evbuffer *buf = evbuffer_new();
		evbuffer_add_printf(buf,
			"%s({type: \"429\", cid: \"%d\", seq: \"0\", content: \"Too Many Requests\"});\n",
			cb,
			cid);
		evhttp_send_reply(req, HTTP_OK, "OK", buf);
		evbuffer_free(buf);
		return 0;
	}
	if(channel->idle == -1){
		this->alloc_channel(channel);
	}
	channel->idle = ServerConfig::channel_idles;

	evhttp_add_header(req->output_headers, "Content-Type", "text/javascript; charset=utf-8");
	evhttp_add_header(req->output_headers, "Connection", "keep-alive");
	evhttp_add_header(req->output_headers, "Cache-Control", "no-cache");
	evhttp_add_header(req->output_headers, "Expires", "0");
	
	if(!channel->msg_list.empty() && channel->seq_next != seq){
		std::vector<std::string>::iterator it = channel->msg_list.end();
		int msg_seq_min = channel->seq_next - channel->msg_list.size();
		if(Channel::SEQ_GT(seq, channel->seq_next) || Channel::SEQ_GT(msg_seq_min, seq)){
			seq = msg_seq_min;
		}
		log_debug("send old msg: [%d, %d]", seq, channel->seq_next - 1);
		it -= (channel->seq_next - seq);

		struct evbuffer *buf = evbuffer_new();
		evbuffer_add_printf(buf, "%s([", cb);
		for(/**/; it != channel->msg_list.end(); it++, seq++){
			std::string &msg = *it;
			evbuffer_add_printf(buf,
				"{type: \"data\", cid: \"%d\", seq: \"%d\", content: \"%s\"}",
				cid,
				seq,
				msg.c_str());
			if(seq != channel->seq_next - 1){
				evbuffer_add(buf, ",", 1);
			}
		}
		evbuffer_add_printf(buf, "]);\n");
		evhttp_send_reply(req, HTTP_OK, "OK", buf);
		evbuffer_free(buf);
		return 0;
	}
	
	Subscriber *sub = sub_pool.alloc();
	sub->req = req;
	sub->serv = this;
	sub->idle = 0;
	sub->noop_seq = noop;
	sub->callback = cb;
	
	channel->add_subscriber(sub);
	subscribers ++;
	log_debug("%s:%d sub %d, channels: %d, subs: %d",
		sub->req->remote_host, sub->req->remote_port,
		channel->id, used_channels.size, channel->subs.size);

	evhttp_send_reply_start(req, HTTP_OK, "OK");
	evhttp_connection_set_closecb(req->evcon, on_connection_close, sub);
	return 0;
}

int Server::ping(struct evhttp_request *req){
	HttpQuery query(req);
	const char *cb = query.get_str("cb", DEFAULT_JSONP_CALLBACK);

	evhttp_add_header(req->output_headers, "Content-Type", "text/javascript; charset=utf-8");
	evhttp_add_header(req->output_headers, "Cache-Control", "no-cache");
	evhttp_add_header(req->output_headers, "Expires", "0");
	struct evbuffer *buf = evbuffer_new();
	evbuffer_add_printf(buf,
		"%s({type: \"ping\", sub_timeout: %d});\n",
		cb,
		ServerConfig::polling_timeout);
	evhttp_send_reply(req, HTTP_OK, "OK", buf);
	evbuffer_free(buf);
	return 0;
}

int Server::pub(struct evhttp_request *req){
	if(evhttp_request_get_command(req) != EVHTTP_REQ_GET){
		evhttp_send_reply(req, 405, "Invalid Method", NULL);
		return 0;
	}
	
	HttpQuery query(req);
	int cid = query.get_int("cid", -1);
	const char *cb = query.get_str("cb", NULL);
	std::string cname = query.get_str("cname", "");
	const char *content = query.get_str("content", "");
	
	Channel *channel = NULL;
	if(cid >= 0){
		channel = this->get_channel(cid);
	}else if(!cname.empty()){
		channel = this->get_channel_by_name(cname);
	}
	if(!channel || channel->idle == -1){
		struct evbuffer *buf = evbuffer_new();
		if(cid >= 0){
			log_trace("channel[%d] not connected, pub content: %s", cid, content);
			evbuffer_add_printf(buf, "channel[%d] not connected\n", cid);
		}else{
			log_trace("cname[%s] not connected, pub content: %s", cname.c_str(), content);
			evbuffer_add_printf(buf, "cname[%s] not connected\n", cname.c_str());
		}
		evhttp_send_reply(req, 404, "Not Found", buf);
		evbuffer_free(buf);
		return 0;
	}
	log_debug("ch: %d, subs: %d, pub content: %s", channel->id, channel->subs.size, content);
		
	// response to publisher
	evhttp_add_header(req->output_headers, "Content-Type", "text/javascript; charset=utf-8");
	struct evbuffer *buf = evbuffer_new();
	if(cb){
		evbuffer_add_printf(buf, "%s(", cb);
	}
	evbuffer_add_printf(buf, "{type: \"ok\"}");
	if(cb){
		evbuffer_add(buf, ");\n", 3);
	}else{
		evbuffer_add(buf, "\n", 1);
	}
	evhttp_send_reply(req, 200, "OK", buf);
	evbuffer_free(buf);

	// push to subscribers
	channel->send("data", content);
	return 0;
}


int Server::sign(struct evhttp_request *req){
	HttpQuery query(req);
	int expires = query.get_int("expires", -1);
	const char *cb = query.get_str("cb", NULL);
	std::string cname = query.get_str("cname", "");
	
	if(expires <= 0){
		expires = ServerConfig::channel_timeout;
	}
	
	Channel *channel = this->get_channel_by_name(cname);
	if(!channel && !free_channels.empty()){
		channel = free_channels.head;
		channel->name = cname;
		this->alloc_channel(channel);
	}	
	if(!channel){
		struct evbuffer *buf = evbuffer_new();
		evbuffer_add_printf(buf, "Invalid channel for cname: %s\n", cname.c_str());
		evhttp_send_reply(req, 404, "Not Found", buf);
		evbuffer_free(buf);
		return 0;
	}

	if(channel->token.empty()){
		channel->create_token();
	}
	if(channel->idle == -1){
		log_debug("sign cname:%s, cid:%d, t:%s, expires:%d",
			cname.c_str(), channel->id, channel->token.c_str(), expires);
	}else{
		log_debug("re-sign cname:%s, cid:%d, t:%s, expires:%d",
			cname.c_str(), channel->id, channel->token.c_str(), expires);
	}
	channel->idle = expires/CHANNEL_CHECK_INTERVAL;

	evhttp_add_header(req->output_headers, "Content-Type", "text/html; charset=utf-8");
	struct evbuffer *buf = evbuffer_new();
	if(cb){
		evbuffer_add_printf(buf, "%s(", cb);
	}
	evbuffer_add_printf(buf,
		"{type: \"sign\", cid: %d, seq: %d, token: \"%s\", expires: %d, sub_timeout: %d}",
		channel->id,
		channel->msg_seq_min(),
		channel->token.c_str(),
		expires,
		ServerConfig::channel_timeout);
	if(cb){
		evbuffer_add(buf, ");\n", 3);
	}else{
		evbuffer_add(buf, "\n", 1);
	}
	evhttp_send_reply(req, 200, "OK", buf);
	evbuffer_free(buf);

	return 0;
}

int Server::close(struct evhttp_request *req){
	HttpQuery query(req);
	std::string cname = query.get_str("cname", "");
	const char *content = query.get_str("content", "");
	
	Channel *channel = this->get_channel_by_name(cname);
	if(!channel){
		log_warn("channel %s not found", cname.c_str());
		struct evbuffer *buf = evbuffer_new();
		evbuffer_add_printf(buf, "cname[%s] not connected\n", cname.c_str());
		evhttp_send_reply(req, 404, "Not Found", buf);
		evbuffer_free(buf);
		return 0;
	}
	log_debug("close ch: %d, subs: %d, content: %s", channel->id, channel->subs.size, content);
		
	// response to publisher
	evhttp_add_header(req->output_headers, "Content-Type", "text/html; charset=utf-8");
	struct evbuffer *buf = evbuffer_new();
	evbuffer_add_printf(buf, "ok %d\n", channel->seq_next);
	evhttp_send_reply(req, 200, "OK", buf);
	evbuffer_free(buf);

	// push to subscribers
	if(channel->idle != -1){
		channel->send("close", content);
		this->delete_channel(channel);
	}

	return 0;
}

int Server::info(struct evhttp_request *req){
	HttpQuery query(req);
	std::string cname = query.get_str("cname", "");

	evhttp_add_header(req->output_headers, "Content-Type", "text/html; charset=utf-8");
	struct evbuffer *buf = evbuffer_new();
	Channel *channel = this->get_channel_by_name(cname);
	if(!cname.empty()){
		int onlines = channel? channel->subs.size : 0;
		evbuffer_add_printf(buf,
			"{cname: \"%s\", subscribers: %d}\n",
			cname.c_str(),
			onlines);
	}else{
		evbuffer_add_printf(buf,
			"{channels: %d, subscribers: %d}\n",
			used_channels.size,
			subscribers);
	}
	evhttp_send_reply(req, 200, "OK", buf);
	evbuffer_free(buf);

	return 0;
}

int Server::check(struct evhttp_request *req){
	HttpQuery query(req);
	std::string cname = query.get_str("cname", "");

	evhttp_add_header(req->output_headers, "Content-Type", "text/html; charset=utf-8");
	struct evbuffer *buf = evbuffer_new();
	Channel *channel = this->get_channel_by_name(cname);
	if(channel && channel->idle != -1){
		evbuffer_add_printf(buf, "{\"%s\": 1}\n", cname.c_str());
	}else{
		evbuffer_add_printf(buf, "{}\n");
	}
	evhttp_send_reply(req, 200, "OK", buf);
	evbuffer_free(buf);

	return 0;
}
