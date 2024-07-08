#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/predef.h> 
#include<bits/stdc++.h>

#ifdef BOOST_OS_WINDOWS
#define _WIN32_WINNT 0x0501
#if _WIN32_WINNT <= 0x0502 
#define BOOST_ASIO_DISABLE_IOCP
#define BOOST_ASIO_ENABLE_CANCELIO 
#endif
#endif


namespace http_errors {
	enum http_error_codes {
		invalid_response = 1
	};
	class http_errors_category : public boost::system::error_category {
	public:
		const char* name() const BOOST_SYSTEM_NOEXCEPT {
			return "http_errors";
		}

		std::string message(int e) const {
			switch (e) {
			case invalid_response:
				return "Server response cannot be parsed.";
				break;
			default:
				return "Unknown error.";
				break;
			}
		}
	};
	const boost::system::error_category& get_http_errors_category() {
		static http_errors_category cat;
		return cat;
	}

	boost::system::error_code make_error_code(http_error_codes e) {
		return boost::system::error_code(static_cast<int>(e), get_http_errors_category());
	}
}

namespace boost {
	namespace system {
		template<>
		struct is_error_code_enum <http_errors::http_error_codes> {
			BOOST_STATIC_CONSTANT(bool, value = true);
		};
	}
}

class HTTPClient;
class HTTPRequest;
class HTTPResponse;

typedef void(*Callback) (
	const HTTPRequest& request,
	const HTTPResponse& response,
	const boost::system::error_code& ec
);

class HTTPResponse {
	friend class HTTPRequest;
	HTTPResponse(): m_response_stream(&m_response_buf){}
public:
	unsigned int get_status_code() const {
		return m_status_code;
	}

	const std::string& get_status_message() const {
		return m_status_message;
	}

	const std::map<std::string, std::string>& get_headers() {
		return m_headers;
	}

	const std::istream& get_response() const {
		return m_response_stream;
	}
private:
	boost::asio::streambuf& get_response_buf() {
		return m_response_buf;
	}

	void set_status_code(unsigned int status_code) {
		m_status_code = status_code;
	}

	void set_status_message(const std::string& status_message) {
		m_status_message = status_message;
	}

	void add_header(const std::string &name, const std::string& value) {
		m_headers[name] = value;
	}

private:
	unsigned int m_status_code;
	std::string m_status_message;
	std::map<std::string, std::string> m_headers;
	boost::asio::streambuf m_response_buf;
	std::istream m_response_stream;
};


class HTTPRequest {
	friend class HTTPClient;
	static const unsigned int DEFAULT_PORT = 80;
	HTTPRequest(boost::asio::io_context& ios, unsigned int id) :
		m_port(DEFAULT_PORT),
		m_id(id),
		m_callback(nullptr),
		m_sock(ios),
		m_resolver(ios),
		m_was_canceled(0),
		m_ios(ios){}
public:
	void set_host(const std::string& host) {
		m_host = host;
	}

	void set_port(unsigned int port) {
		m_port = port;
	}

	void set_uri(const std::string& uri) {
		m_uri = uri;
	}
	
	void set_callback(Callback callback) {
		m_callback = callback;
	}
	
	std::string get_host() const{
		return m_host;
	}
	
	unsigned get_port() const{
		return m_port;
	}
	
	std::string get_uri() const{
		return m_uri;
	}

	unsigned int get_id() const{
		return m_id;
	}

	void execute() {
		assert(m_port>0);
		assert(m_host.length()>0);
		assert(m_uri.length() > 0);
		assert(m_callback != nullptr);
		boost::asio::ip::tcp::resolver::query resolver_query(
			m_host, std::to_string(m_port), 
			boost::asio::ip::tcp::resolver::query::numeric_service);
		std::unique_lock<std::mutex> lk(m_cancel_mux);
		if (m_was_canceled) {
			lk.unlock();
			on_finish(boost::system::error_code(boost::asio::error::operation_aborted));
			return;
		}
		
		m_resolver.async_resolve(resolver_query, [this](const boost::system::error_code &ec,
			boost::asio::ip::tcp::resolver::iterator it) {
				on_host_name_resolved(ec, it);
			});
		
	}

	void cancel() {
		std::unique_lock<std::mutex> lk;

		m_was_canceled = 1;

		m_resolver.cancel();

		if (m_sock.is_open()) {
			m_sock.cancel();
		}
	}

private:

	void on_host_name_resolved(
		const boost::system::error_code& ec,
		boost::asio::ip::tcp::resolver::iterator iterator)
	{
		if (ec) {
			on_finish(ec);
			return;
		}
		std::unique_lock<std::mutex>
			cancel_lock(m_cancel_mux);
		if (m_was_canceled) {
			cancel_lock.unlock();
			on_finish(boost::system::error_code(
				boost::asio::error::operation_aborted));
			return;
		}

		boost::asio::async_connect(m_sock,
			iterator,
			[this](const boost::system::error_code& ec,
				boost::asio::ip::tcp::resolver::iterator iterator)
			{
				on_connection_established(ec, iterator);
			});
	}
	
	void on_connection_established(const boost::system::error_code& ec,
		boost::asio::ip::tcp::resolver::iterator it) {
		if (ec) {
			std::cout << "Error from on_connection_established \n";
			on_finish(ec);
			return;
		}

		m_request_buf += "GET " + m_uri + " HTTP/1.1\r\n";
		m_request_buf += "Host: " + m_host + "\r\n";
		m_request_buf += "\r\n";
		std::unique_lock<std::mutex> lk(m_cancel_mux);
		
		if (m_was_canceled) {
			lk.unlock();
			on_finish(boost::system::error_code(
				boost::asio::error::operation_aborted));
			return;
		}

		boost::asio::async_write(m_sock, boost::asio::buffer(m_request_buf), 
			[this](const boost::system::error_code& ec,
			std::size_t bytes_transfered) {
				on_request_sent(ec, bytes_transfered);
			});
	}

	void on_request_sent(const boost::system::error_code& ec,
		std::size_t bytes_transfered) {
		if (ec) {
			on_finish(ec);
			return;
		}
		m_sock.shutdown(boost::asio::socket_base::shutdown_send);

		std::unique_lock<std::mutex> lk(m_cancel_mux);
		if (m_was_canceled) {
			lk.unlock();
			on_finish(boost::system::error_code(
				boost::asio::error::operation_aborted));
			return;
		}

		boost::asio::async_read_until(m_sock, m_response.get_response_buf(), 
			"\r\n", [this](const boost::system::error_code& ec, size_t bytes_transfered) {
				on_status_line_received(ec, bytes_transfered);
			});
	}

	void on_status_line_received(const boost::system::error_code &ec, 
		std::size_t bytes_transfered) {
		if (ec) {
			std::cout << "Error from on_status_line_received \n";
			on_finish(ec);
			return;
		}

		std::string http_version, str_status_code, status_message;

		std::istream inn(&m_response.get_response_buf());
		inn >> http_version;
		if (http_version != "HTTP/1.1") {
			on_finish(http_errors::invalid_response);
			return;
		}
		inn >> str_status_code;
		unsigned int status_code = 200;
		try {
			status_code = std::stoul(str_status_code);
		}
		catch (std::logic_error&) {
			on_finish(http_errors::invalid_response);
			return;
		}

		std::getline(inn, status_message, '\r');
		inn.get();
		m_response.set_status_code(status_code);
		m_response.set_status_message(status_message);
		
		std::unique_lock<std::mutex> lk(m_cancel_mux);
		if (m_was_canceled) {
			lk.unlock();
			on_finish(boost::system::error_code(boost::asio::error::operation_aborted));
			return;
		}
		boost::asio::async_read_until(m_sock, m_response.get_response_buf(),
			"\r\n\r\n",
			[this](const boost::system::error_code &ec,
				std::size_t bytes_transfered) {
					on_headers_received(ec, bytes_transfered);
			});
	}

	void on_headers_received(const boost::system::error_code& ec, std::size_t bytes_tranfered) {
		if (ec) {
			std::cout << "Error from on_headers_received \n";
			on_finish(ec);
			return;
		}
		std::string header, header_name, header_value;
		std::istream inn(&m_response.get_response_buf());

		while (1) {
			std::getline(inn, header, '\r');
			inn.get();
			if (header == "") break;

			size_t separator_pos = header.find(':');
			if (separator_pos != std::string::npos) {
				header_name = header.substr(0, separator_pos);
				if (separator_pos < header.length()-1) {
					header_value = header.substr(separator_pos+1);
				}
				else {
					header_value = "";
				}
				m_response.add_header(header_name, header_value);
			}
		}

		std::unique_lock<std::mutex> lk(m_cancel_mux);
		if (m_was_canceled) {
			lk.unlock();
			on_finish(boost::system::error_code(boost::asio::error::operation_aborted));
			return;
		}

		boost::asio::async_read(m_sock, m_response.get_response_buf(),
			[this](const boost::system::error_code& ec, std::size_t bytes_transfered) {
				on_body_received(ec, bytes_transfered);
			});
		return;
	}
	
	void on_body_received(const boost::system::error_code& ec, std::size_t bytes_transfered) {
		if (ec == boost::asio::error::eof) {
			std::cout << "Error from on_body_received \n";
			on_finish(boost::system::error_code());
		}
		else {
			on_finish(ec);
		}
	}

	void on_finish(const boost::system::error_code &ec) {
		if (ec) {
			std::cout << "error ocured: "<< ec.value()<<
				". Message: " <<ec.message()<< '\n';
		}

		m_callback(*this, m_response, ec);
		return;
	}


	unsigned int m_port, m_id;
	std::string m_host, m_uri, m_request_buf;
	Callback m_callback;
	boost::asio::ip::tcp::socket m_sock;
	boost::asio::ip::tcp::resolver m_resolver;
	bool m_was_canceled;
	boost::asio::io_context& m_ios;
	HTTPResponse m_response;
	std::mutex m_cancel_mux;
};

class HTTPClient {
public:
	HTTPClient() {
		m_work.reset(new boost::asio::io_context::work(m_ios));

		m_thread.reset(new std::thread([this]() {
			m_ios.run();
		}));
	}

	std::shared_ptr<HTTPRequest> create_request(unsigned int id) {
		return std::shared_ptr<HTTPRequest>(
			new HTTPRequest(m_ios, id));
	}

	void close() {
		m_work.reset(NULL);

		m_thread->join();
	}
private:
	boost::asio::io_context m_ios;
	std::unique_ptr<boost::asio::io_context::work> m_work;
	std::unique_ptr<std::thread> m_thread;
};

void handler(const HTTPRequest& request, const HTTPResponse& response, 
	const boost::system::error_code &ec) {

	if (!ec) {
	std::cout << "Request #" << request.get_id() << " has completed. Response: " <<
		response.get_response().rdbuf();
	}
	else if (ec == boost::asio::error::operation_aborted) {
		std::cout << "Request #" << request.get_id() << " has been canceled by the user.\n";
	}
	else {
		std::cout << "Request #" << request.get_id() << " failed! Error code = " << ec.value() <<
			". Error message = " << ec.message()<<'\n';
	}
	return;
}

int main() {
	try {
		HTTPClient client;
		std::shared_ptr<HTTPRequest> request_one = client.create_request(1);

		request_one->set_host("localhost");
		request_one->set_uri("/index.html");
		request_one->set_port(3333);
		request_one->set_callback(handler);

		request_one->execute();

		std::shared_ptr<HTTPRequest> request_two = client.create_request(2);

		request_two->set_host("localhost");
		request_two->set_uri("/index.html");
		request_two->set_port(3333);
		request_two->set_callback(handler);

		request_two->execute();


		std::this_thread::sleep_for(std::chrono::seconds(15));
		
		client.close();
	}
	catch (boost::system::error_code &ec) {
		std::cout << "Error ocured! Error code = " << ec.value()<<
			". Message: "<<ec.what();
		return ec.value();
	}
	
	return 0;
}