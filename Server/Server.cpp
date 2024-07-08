#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/predef.h> 
#include<boost/filesystem.hpp>
#include<bits/stdc++.h>


class Service {
	static const std::map<unsigned int, std::string>
		http_status_table;
public:
	Service(std::shared_ptr<boost::asio::ip::tcp::socket> sock) :
		m_sock(sock), 
		m_request(4096), 
		m_response_status_code(200),
		m_resourse_size_bytes(0) {};

	void start_handling() {
		boost::asio::async_read_until(*m_sock.get(), m_request, 
			"\r\n", 
			[this](const boost::system::error_code& ec,
				std::size_t bytes_transfered) {
					on_request_line_received(ec, bytes_transfered);
			});
	}
private:
	void on_request_line_received(const boost::system::error_code &ec,
		std::size_t bytes_transfered) {
		if(ec){
			std::cout << "Error ocured! Error code = " << ec.value() << ". Message: " << ec.message();
			if (ec == boost::asio::error::not_found) {
				m_response_status_code = 413;
				send_response();
				return;
			}
			else {
				on_finish();
				return;
			}
		}

		std::string request_line;
		std::istream inn(&m_request);

		std::getline(inn, request_line, '\r');
		inn.get();

		std::string request_method;
		std::istringstream request_line_stream(request_line);
		request_line_stream >> request_method;
		
		if (request_method.compare("GET") != 0) {
			m_response_status_code = 501;
			send_response();
			return;
		}

		request_line_stream >> m_request_resourse;

		std::string request_http_version;
		request_line_stream >> request_http_version;

		if (request_http_version.compare("HTTP/1.1") != 0) {
			m_response_status_code = 505;
			send_response();
			return;
		}

		boost::asio::async_read_until(*m_sock.get(), m_request, "\r\n\r\n",
			[this](const boost::system::error_code &ec, std::size_t bytes_transfered) {
				on_headers_received(ec, bytes_transfered);
			});

		return;
	}

	void on_headers_received(const boost::system::error_code& ec, std::size_t bytes_transfered) {
		if (ec) {
			std::cout << "Error ocured! Error code = " << ec.value() << ". Message: " << ec.message();
			if (ec == boost::asio::error::not_found) {
				m_response_status_code = 413;
				send_response();
				return;
			}
			else {
				on_finish();
				return;
			}
		}

		std::istream request_stream(&m_request);
		std::string header_name, header_value;

		while (!request_stream.eof()) {
			std::getline(request_stream, header_name, ':');
			if (!request_stream.eof()) {
				std::getline(request_stream, header_value, '\r');

				request_stream.get();
				m_request_headers[header_name] = header_value;
			}
		}

		process_request();
		send_response();

		return;
	}

	void process_request() {
		std::string resource_file_path = std::string("C:\\http_root") +
			m_request_resourse;
		if (!boost::filesystem::exists(resource_file_path)) {
			m_response_status_code = 404;

			return;
		}

		std::ifstream resourse_fstream(resource_file_path, std::ifstream::binary);
		
		if (!resourse_fstream.is_open()) {
			m_response_status_code = 500;
			return;
		}

		resourse_fstream.seekg(0, std::ifstream::end);
		m_resourse_size_bytes = static_cast<std::size_t>(resourse_fstream.tellg());
		
		m_resourse_buffer.reset(new char[m_resourse_size_bytes]);

		resourse_fstream.seekg(0, std::ifstream::beg);
		resourse_fstream.read(m_resourse_buffer.get(), 
		m_resourse_size_bytes);

		m_response_headers += std::string("content-length")+": "+std::to_string(m_resourse_size_bytes)+"\r\n";
	}

	void send_response() {
		m_sock->shutdown(boost::asio::ip::tcp::socket::shutdown_receive);

		auto status_line = http_status_table.at(m_response_status_code);

		m_response_status_line = std::string("HTTP/1.1 ") + status_line + "\r\n";
		
		m_response_headers += "\r\n";

		std::vector<boost::asio::const_buffer> response_buffers;
		response_buffers.push_back(boost::asio::buffer(m_response_status_line));
		
		if (m_response_headers.length() > 0) {
			response_buffers.push_back(boost::asio::buffer(m_response_headers));
		}
		
		if (m_resourse_size_bytes > 0) {
			response_buffers.push_back(boost::asio::buffer(m_resourse_buffer.get(), m_resourse_size_bytes));
		}

		boost::asio::async_write(*m_sock.get(), response_buffers,
			[this](const boost::system::error_code& ec,
				std::size_t bytes_transferred) {
					on_response_sent(ec, bytes_transferred);
			});
	}

	void on_response_sent(const boost::system::error_code& ec,
		std::size_t bytes_transferred) {
		if (ec) {
			std::cout << "Error ocured! Error code = " << ec.value() << ". Message: " << ec.message();
		}
		m_sock->shutdown(boost::asio::ip::tcp::socket::shutdown_both);

		on_finish();
	}
	
	void on_finish() {
		delete this;
	}


	std::shared_ptr<boost::asio::ip::tcp::socket> m_sock;
	boost::asio::streambuf m_request;
	std::map<std::string, std::string> m_request_headers;
	std::string m_request_resourse;

	std::unique_ptr<char[]> m_resourse_buffer;
	unsigned int m_response_status_code;
	std::size_t m_resourse_size_bytes;
	std::string m_response_headers;
	std::string m_response_status_line;
};

const std::map<unsigned int, std::string> Service::http_status_table = {
		{200, "200 OK"},
		{404, "404 NOt Found"},
		{413, "413 Request Entity Too Large"},
		{500, "500 Server Error"},
		{501, "501 Not Implemented"},
		{505, "505 HTTP Version Not Supported"}
};

class Acceptor {
public:
	Acceptor(boost::asio::io_context& ios, unsigned short port)
		: m_ios(ios),
		m_acceptor(m_ios, 
			boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::any(), 
				port)),
		m_isStopped(false) {}
	void start() {
		m_acceptor.listen();
		init_accept();
	}

	void stop() {
		m_isStopped.store(1);
	}

private:
	void init_accept() {
		std::shared_ptr<boost::asio::ip::tcp::socket> sock(new boost::asio::ip::tcp::socket(m_ios));
		m_acceptor.async_accept(*sock.get(),
			[this, sock](const boost::system::error_code& ec) {
				on_accept(ec, sock);
			});
	}

	void on_accept(const boost::system::error_code& ec,
		std::shared_ptr<boost::asio::ip::tcp::socket> sock)
	{
		if (!ec) {
			(new Service(sock))->start_handling();
		}
		else {
			std::cout << "Error occured! Error code = "
				<< ec.value()
				<< ". Message: " << ec.message();
		}
		// Init next async accept operation if
		// acceptor has not been stopped yet.
		if (!m_isStopped.load()) {
			init_accept();
		}
		else {
			// Stop accepting incoming connections
			// and free allocated resources.
			m_acceptor.close();
		}
	}
private:
	boost::asio::io_service& m_ios;
	boost::asio::ip::tcp::acceptor m_acceptor;
	std::atomic<bool>m_isStopped;
};

class Server {
public:
	Server() {
		m_work.reset(new boost::asio::io_service::work(m_ios));
	}
	// Start the server.
	void Start(unsigned short port_num,
		unsigned int thread_pool_size) {

		assert(thread_pool_size > 0);
		// Create and start Acceptor.
		acc.reset(new Acceptor(m_ios, port_num));
		acc->start();
		// Create specified number of threads and
		// add them to the pool.
		for (unsigned int i = 0; i < thread_pool_size; i++) {
			std::unique_ptr<std::thread> th(
				new std::thread([this]()
					{
						m_ios.run();
					}));
			m_thread_pool.push_back(std::move(th));
		}
	}
	// Stop the server.
	void Stop() {
		acc->stop();
		m_ios.stop();
		for (auto& th : m_thread_pool) {
			th->join();
		}
	}
private:
	boost::asio::io_service m_ios;
	std::unique_ptr<boost::asio::io_service::work>m_work;
	std::unique_ptr<Acceptor>acc;
	std::vector<std::unique_ptr<std::thread>>m_thread_pool;
};

const unsigned int DEFAULT_THREAD_POOL_SIZE = 2;
int main()
{
	unsigned short port_num = 3333;
	try {
		Server srv;
		unsigned int thread_pool_size =
			std::thread::hardware_concurrency() * 2;

		if (thread_pool_size == 0)
			thread_pool_size = DEFAULT_THREAD_POOL_SIZE;
		srv.Start(port_num, thread_pool_size);
		std::this_thread::sleep_for(std::chrono::seconds(60));
		srv.Stop();
	}
	catch (boost::system::system_error& e) {
		std::cout << "Error occured! Error code = "
			<< e.code() << ". Message: "
			<< e.what();
	}
	return 0;
}