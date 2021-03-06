#include "HttpClient.hpp"
#include "Util.hpp"
#include "Timer.hpp"
#include <iostream>
#include <sstream>
#include <string>
#include <stdexcept>
#include "3rdparty/uri-parser/UriParser.hpp"
#include "3rdparty/base64/base64.h"

using namespace Kite;


class Kite::HttpClientPrivate
{
public:
    HttpClientPrivate()
    {
        state = 0;
        responseCode = 999;
    }

    void setUrl(const std::string &url, const std::string &verb)
    {
        d_path = "/";
        d_host.clear();
        d_verb = verb;
        p_buf.clear();
        responseCode = 999;
        d_content_length = 0;
        responseHeaders.clear();
        d_post_body.clear();


        std::string url_(url);
        http::url parsed = http::ParseHttpUrl(url_);

        d_port = parsed.port;
        if (parsed.protocol == "http") {
            d_is_https = false;
            if (d_port == 0) {
                d_port = 80;
            }
        } else if (parsed.protocol == "https") {
            d_is_https = true;
            if (d_port == 0) {
                d_port = 443;
            }
        } else {
            throw std::invalid_argument("unsupported url schema " + parsed.protocol);
        }

        d_host = parsed.host;
        d_path = parsed.path;

        if (parsed.user.empty()) {
            d_auth = std::string();
        } else {
            d_auth = std::string();
            if (!Base64::Encode(parsed.user + ":" + parsed.password, &d_auth)) {
                throw std::invalid_argument("base64 on basic auth failed");
            }
            d_auth = "Basic " + d_auth;
        }

    }

    void onLine(const std::string &line)
    {
        if (state == Kite::HttpClient::Connected) {
            std::istringstream ss(line);
            std::string httpd;
            ss >> httpd;
            ss >> responseCode;
            state = Kite::HttpClient::StatusCompleted;
        } else {
            if (line == "\r\n") {
                state = HttpClient::HeaderCompleted;
                p->onHeadersReady(responseHeaders);
            } else {
                std::istringstream ss(line);
                std::string key;
                std::getline(ss, key, ':');
                std::string value(std::istreambuf_iterator<char>(ss), {});

                Kite::trim(key);
                Kite::trim(value);

                if (key == "Content-Length") {
                    d_content_length = std::stoi(value);
                }

                responseHeaders[key] = value;
            }
        }
    }

    std::map<std::string,std::string> requestHeaders;
    std::map<std::string,std::string> responseHeaders;

    int responseCode;
    int d_content_length;
    HttpClient::Status status;
    int state;

    std::string d_verb;
    bool        d_is_https;
    std::string d_host;
    int         d_port;
    std::string d_path;
    std::string d_post_body;
    std::string d_auth;
    friend class HttpClient;

    std::string p_buf;


    HttpClient *p;
};


///////////////////////////////////////////////////////////////////////////////


HttpClient::HttpClient(std::weak_ptr<Kite::EventLoop> ev)
    : Kite::SecureSocket(ev)
    , p(new HttpClientPrivate)
{
    p->p = this;
}

HttpClient::~HttpClient()
{
    delete p;
}

void HttpClient::get(const std::string &url)
{
    p->setUrl(url, "GET");
    p->status = Kite::HttpClient::Connecting;

    connect(p->d_host, p->d_port, 5000, p->d_is_https);
}

void HttpClient::post(const std::string &url, const std::string &body)
{
    p->setUrl(url, "POST");
    p->status = Kite::HttpClient::Connecting;
    p->d_post_body = body;

    connect(p->d_host, p->d_port, 5000, p->d_is_https);
}


void HttpClient::onActivated(int)
{
    if (p->state < HttpClient::HeaderCompleted) {
        if (p->p_buf.length() >= 4048) {
            std::cerr << "warning: internal HttpClient buffer overflow" << std::endl;
            disconnect();
            return;
        }
        char c;
        int len = read(&c, 1);
        if (len != 1) {
            disconnect();
            return;
        }

        p->p_buf += c;
        if (c == '\n') {
            p->onLine(p->p_buf);
            p->p_buf.clear();
        }
        //return get activated again.
        //TODO overhead ok for header?
    } else {
        onReadActivated();
    }
}


void HttpClient::onReadActivated()
{
    char buf[1024];
    int len = read(buf, 1024);
    if (len < 1) {
        disconnect();
        return;
    }
    if (p->p_buf.length() + len >= 4048) {
        throw std::runtime_error("overflow");
    }
    p->p_buf += std::string(buf, len);
}

void HttpClient::onDisconnected(SocketState state)
{
    onFinished(p->status, p->responseCode, p->p_buf);
}

void HttpClient::onConnected() {
    p->state =  Kite::HttpClient::Connected;

    std::stringstream ss;

    ss << p->d_verb << " " << p->d_path << " HTTP/1.1\r\n";
    ss << "Host: " << p->d_host << "\r\n";

    for (auto it = p->requestHeaders.begin(); it != p->requestHeaders.end(); ++it) {
        ss << it->first << ": " << it->second << "\r\n";
    }

    if (p->d_verb == "POST") {
        ss << "Content-Length: " << p->d_post_body.length() << "\r\n";
    }
    if (!p->d_auth.empty()) {
        ss << "Authorization: " << p->d_auth << "\r\n";
    }

    ss << "Connection: close\r\n";
    ss << "\r\n";
    ss << p->d_post_body;

    write(ss.str().c_str(), ss.str().length());
}


void HttpClient::setHeaders(std::map<std::string,std::string> requestHeaders)
{
    p->requestHeaders = requestHeaders;
}


