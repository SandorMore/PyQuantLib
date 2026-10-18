#include <optional>
#include <string>
#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;
using namespace boost::beast;
using namespace boost::beast::websocket;

class WS_Connection final 
{
public:
    asio::awaitable<void> ws_client(std::string host, std::string port, std::string target, std::string subscirbe_msg);
    
private:
};