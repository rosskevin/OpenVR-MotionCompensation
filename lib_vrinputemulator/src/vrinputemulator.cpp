#include <vrinputemulator.h>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <config.h>


#if VRINPUTEMULATOR_EASYLOGGING == 1
#include "logging.h";
#define WRITELOG(level, txt) LOG(level) << txt;
#else
#define WRITELOG(level, txt) std::cerr << txt;
#endif



namespace vrinputemulator
{
// Receives and dispatches ipc messages
	void VRInputEmulator::_ipcThreadFunc(VRInputEmulator* _this)
	{
		_this->_ipcThreadRunning = true;
		while (!_this->_ipcThreadStop)
		{
			try
			{
				ipc::Reply message;
				uint64_t recv_size;
				unsigned priority;
				boost::posix_time::ptime timeout = boost::posix_time::microsec_clock::universal_time() + boost::posix_time::milliseconds(50);
				if (_this->_ipcClientQueue->timed_receive(&message, sizeof(ipc::Reply), recv_size, priority, timeout))
				{
					if (recv_size == sizeof(ipc::Reply))
					{
						std::lock_guard<std::recursive_mutex> lock(_this->_mutex);
						auto i = _this->_ipcPromiseMap.find(message.messageId);
						if (i != _this->_ipcPromiseMap.end())
						{
							if (i->second.isValid)
							{
								i->second.promise.set_value(message);
							}
							else
							{
								_this->_ipcPromiseMap.erase(i); // nobody wants it, so we delete it
							}
						}
					}
				}
				else
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}
			catch (std::exception & ex)
			{
				WRITELOG(ERROR, "Exception in ipc receive loop: " << ex.what() << std::endl);
			}
		}
		_this->_ipcThreadRunning = false;
	}


	VRInputEmulator::VRInputEmulator(const std::string& serverQueue, const std::string& clientQueue) : _ipcServerQueueName(serverQueue), _ipcClientQueueName(clientQueue)
	{
	}

	VRInputEmulator::~VRInputEmulator()
	{
		disconnect();
	}

	bool VRInputEmulator::isConnected() const
	{
		return _ipcServerQueue != nullptr;
	}

	void VRInputEmulator::connect()
	{
		if (!_ipcServerQueue)
		{
		// Open server-side message queue
			try
			{
				_ipcServerQueue = new boost::interprocess::message_queue(boost::interprocess::open_only, _ipcServerQueueName.c_str());
			}
			catch (std::exception & e)
			{
				_ipcServerQueue = nullptr;
				std::stringstream ss;
				ss << "Could not open server-side message queue: " << e.what();
				throw vrinputemulator_connectionerror(ss.str());
			}
			// Append random number to client queue name (and hopefully no other client uses the same random number)
			_ipcClientQueueName += std::to_string(_ipcRandomDist(_ipcRandomDevice));
			// Open client-side message queue
			try
			{
				boost::interprocess::message_queue::remove(_ipcClientQueueName.c_str());
				_ipcClientQueue = new boost::interprocess::message_queue(
					boost::interprocess::create_only,
					_ipcClientQueueName.c_str(),
					100,					//max message number
					sizeof(ipc::Reply)    //max message size
				);
			}
			catch (std::exception & e)
			{
				delete _ipcServerQueue;
				_ipcServerQueue = nullptr;
				_ipcClientQueue = nullptr;
				std::stringstream ss;
				ss << "Could not open client-side message queue: " << e.what();
				throw vrinputemulator_connectionerror(ss.str());
			}
			// Start ipc thread
			_ipcThreadStop = false;
			_ipcThread = std::thread(_ipcThreadFunc, this);
			// Send ClientConnect message to server
			ipc::Request message(ipc::RequestType::IPC_ClientConnect);
			auto messageId = _ipcRandomDist(_ipcRandomDevice);
			message.msg.ipc_ClientConnect.messageId = messageId;
			message.msg.ipc_ClientConnect.ipcProcotolVersion = IPC_PROTOCOL_VERSION;
			strncpy_s(message.msg.ipc_ClientConnect.queueName, _ipcClientQueueName.c_str(), 127);
			message.msg.ipc_ClientConnect.queueName[127] = '\0';
			std::promise<ipc::Reply> respPromise;
			auto respFuture = respPromise.get_future();
			{
				std::lock_guard<std::recursive_mutex> lock(_mutex);
				_ipcPromiseMap.insert({ messageId, std::move(respPromise) });
			}
			_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
			// Wait for response
			auto resp = respFuture.get();
			m_clientId = resp.msg.ipc_ClientConnect.clientId;
			{
				std::lock_guard<std::recursive_mutex> lock(_mutex);
				_ipcPromiseMap.erase(messageId);
			}
			if (resp.status != ipc::ReplyStatus::Ok)
			{
				delete _ipcServerQueue;
				_ipcServerQueue = nullptr;
				delete _ipcClientQueue;
				_ipcClientQueue = nullptr;
				std::stringstream ss;
				ss << "Connection rejected by server: ";
				if (resp.status == ipc::ReplyStatus::InvalidVersion)
				{
					ss << "Incompatible ipc protocol versions (server: " << resp.msg.ipc_ClientConnect.ipcProcotolVersion << ", client: " << IPC_PROTOCOL_VERSION << ")";
					throw vrinputemulator_invalidversion(ss.str());
				}
				else if (resp.status != ipc::ReplyStatus::Ok)
				{
					ss << "Error code " << (int)resp.status;
					throw vrinputemulator_connectionerror(ss.str());
				}
			}
		}
	}

	void VRInputEmulator::disconnect()
	{
		if (_ipcServerQueue)
		{
// Send disconnect message (so the server can free resources)
			ipc::Request message(ipc::RequestType::IPC_ClientDisconnect);
			auto messageId = _ipcRandomDist(_ipcRandomDevice);
			message.msg.ipc_ClientDisconnect.clientId = m_clientId;
			message.msg.ipc_ClientDisconnect.messageId = messageId;
			std::promise<ipc::Reply> respPromise;
			auto respFuture = respPromise.get_future();
			{
				std::lock_guard<std::recursive_mutex> lock(_mutex);
				_ipcPromiseMap.insert({ messageId, std::move(respPromise) });
			}
			_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
			auto resp = respFuture.get();
			m_clientId = resp.msg.ipc_ClientConnect.clientId;
			{
				std::lock_guard<std::recursive_mutex> lock(_mutex);
				_ipcPromiseMap.erase(messageId);
			}
			// Stop ipc thread
			if (_ipcThreadRunning)
			{
				_ipcThreadStop = true;
				_ipcThread.join();
			}
			// delete message queues
			if (_ipcServerQueue)
			{
				delete _ipcServerQueue;
				_ipcServerQueue = nullptr;
			}
			if (_ipcClientQueue)
			{
				delete _ipcClientQueue;
				_ipcClientQueue = nullptr;
			}
		}
	}

	void VRInputEmulator::ping(bool modal, bool enableReply)
	{
		if (_ipcServerQueue)
		{
			uint32_t messageId = _ipcRandomDist(_ipcRandomDevice);
			uint64_t nonce = _ipcRandomDist(_ipcRandomDevice);
			ipc::Request message(ipc::RequestType::IPC_Ping);
			message.msg.ipc_Ping.clientId = m_clientId;
			message.msg.ipc_Ping.messageId = messageId;
			message.msg.ipc_Ping.nonce = nonce;
			if (modal)
			{
				std::promise<ipc::Reply> respPromise;
				auto respFuture = respPromise.get_future();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.insert({ messageId, std::move(respPromise) });
				}
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
				auto resp = respFuture.get();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.erase(messageId);
				}
				if (resp.status != ipc::ReplyStatus::Ok)
				{
					std::stringstream ss;
					ss << "Error while pinging server: Error code " << (int)resp.status;
					throw vrinputemulator_exception(ss.str());
				}
			}
			else
			{
				if (enableReply)
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					message.msg.ipc_Ping.messageId = messageId;
					_ipcPromiseMap.insert({ messageId, _ipcPromiseMapEntry() });
				}
				else
				{
					message.msg.ipc_Ping.messageId = 0;
				}
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
			}
		}
		else
		{
			throw vrinputemulator_connectionerror("No active connection.");
		}
	}


	void VRInputEmulator::openvrVendorSpecificEvent(uint32_t deviceId, vr::EVREventType eventType, const vr::VREvent_Data_t& eventData, double timeOffset)
	{
		if (_ipcServerQueue)
		{
			ipc::Request message(ipc::RequestType::OpenVR_VendorSpecificEvent);
			message.msg.ovr_VendorSpecificEvent.deviceId = deviceId;
			message.msg.ovr_VendorSpecificEvent.eventType = eventType;
			message.msg.ovr_VendorSpecificEvent.eventData = eventData;
			message.msg.ovr_VendorSpecificEvent.timeOffset = timeOffset;
			_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
		}
		else
		{
			throw vrinputemulator_connectionerror("No active connection.");
		}
	}

	void VRInputEmulator::getDeviceInfo(uint32_t deviceId, DeviceInfo& info)
	{
		if (_ipcServerQueue)
		{
			ipc::Request message(ipc::RequestType::DeviceManipulation_GetDeviceInfo);
			memset(&message.msg, 0, sizeof(message.msg));
			message.msg.ovr_GenericDeviceIdMessage.clientId = m_clientId;
			message.msg.ovr_GenericDeviceIdMessage.deviceId = deviceId;
			uint32_t messageId = _ipcRandomDist(_ipcRandomDevice);
			message.msg.ovr_GenericDeviceIdMessage.messageId = messageId;
			std::promise<ipc::Reply> respPromise;
			auto respFuture = respPromise.get_future();
			{
				std::lock_guard<std::recursive_mutex> lock(_mutex);
				_ipcPromiseMap.insert({ messageId, std::move(respPromise) });
			}
			_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
			auto resp = respFuture.get();
			{
				std::lock_guard<std::recursive_mutex> lock(_mutex);
				_ipcPromiseMap.erase(messageId);
			}
			std::stringstream ss;
			ss << "Error while getting device info: ";
			if (resp.status == ipc::ReplyStatus::Ok)
			{
				info.deviceId = resp.msg.dm_deviceInfo.deviceId;
				info.deviceClass = resp.msg.dm_deviceInfo.deviceClass;
				info.deviceMode = resp.msg.dm_deviceInfo.deviceMode;
			}
			else if (resp.status == ipc::ReplyStatus::InvalidId)
			{
				ss << "Invalid device id";
				throw vrinputemulator_invalidid(ss.str());
			}
			else if (resp.status == ipc::ReplyStatus::NotFound)
			{
				ss << "Device not found";
				throw vrinputemulator_notfound(ss.str());
			}
			else if (resp.status != ipc::ReplyStatus::Ok)
			{
				ss << "Error code " << (int)resp.status;
				throw vrinputemulator_exception(ss.str());
			}
		}
		else
		{
			throw vrinputemulator_connectionerror("No active connection.");
		}
	}

	void VRInputEmulator::setDeviceNormalMode(uint32_t deviceId, bool modal)
	{
		if (_ipcServerQueue)
		{
			ipc::Request message(ipc::RequestType::DeviceManipulation_DefaultMode);
			memset(&message.msg, 0, sizeof(message.msg));
			message.msg.ovr_GenericDeviceIdMessage.clientId = m_clientId;
			message.msg.ovr_GenericDeviceIdMessage.messageId = 0;
			message.msg.ovr_GenericDeviceIdMessage.deviceId = deviceId;
			if (modal)
			{
				uint32_t messageId = _ipcRandomDist(_ipcRandomDevice);
				std::promise<ipc::Reply> respPromise;
				auto respFuture = respPromise.get_future();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.insert({ messageId, std::move(respPromise) });
				}
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
				auto resp = respFuture.get();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.erase(messageId);
				}
				std::stringstream ss;
				ss << "Error while setting normal mode: ";
				if (resp.status == ipc::ReplyStatus::InvalidId)
				{
					ss << "Invalid device id";
					throw vrinputemulator_invalidid(ss.str());
				}
				else if (resp.status == ipc::ReplyStatus::NotFound)
				{
					ss << "Device not found";
					throw vrinputemulator_notfound(ss.str());
				}
				else if (resp.status != ipc::ReplyStatus::Ok)
				{
					ss << "Error code " << (int)resp.status;
					throw vrinputemulator_exception(ss.str());
				}
			}
			else
			{
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
			}
		}
		else
		{
			throw vrinputemulator_connectionerror("No active connection.");
		}
	}

	void VRInputEmulator::setDeviceMotionCompensationMode(uint32_t deviceId, MotionCompensationVelAccMode velAccMode, bool modal)
	{
		bool retval = false;
		if (_ipcServerQueue)
		{
			ipc::Request message(ipc::RequestType::DeviceManipulation_MotionCompensationMode);
			memset(&message.msg, 0, sizeof(message.msg));
			message.msg.dm_MotionCompensationMode.clientId = m_clientId;
			message.msg.dm_MotionCompensationMode.messageId = 0;
			message.msg.dm_MotionCompensationMode.deviceId = deviceId;
			message.msg.dm_MotionCompensationMode.velAccCompensationMode = velAccMode;
			if (modal)
			{
				uint32_t messageId = _ipcRandomDist(_ipcRandomDevice);
				message.msg.dm_MotionCompensationMode.messageId = messageId;
				std::promise<ipc::Reply> respPromise;
				auto respFuture = respPromise.get_future();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.insert({ messageId, std::move(respPromise) });
				}
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
				WRITELOG(INFO, "MC message created sending to driver" << std::endl);
				auto resp = respFuture.get();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.erase(messageId);
				}
				std::stringstream ss;
				ss << "Error while setting motion compensation mode: ";
				if (resp.status == ipc::ReplyStatus::InvalidId)
				{
					ss << "Invalid device id";
					throw vrinputemulator_invalidid(ss.str(), (int)resp.status);
				}
				else if (resp.status == ipc::ReplyStatus::NotFound)
				{
					ss << "Device not found";
					throw vrinputemulator_notfound(ss.str(), (int)resp.status);
				}
				else if (resp.status != ipc::ReplyStatus::Ok)
				{
					ss << "Error code " << (int)resp.status;
					throw vrinputemulator_exception(ss.str(), (int)resp.status);
				}
			}
			else
			{
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
				WRITELOG(INFO, "MC message created sending to driver" << std::endl);
			}
		}
		else
		{
			throw vrinputemulator_connectionerror("No active connection.");
		}
	}


	void VRInputEmulator::setMotionVelAccCompensationMode(MotionCompensationVelAccMode velAccMode, bool modal)
	{
		if (_ipcServerQueue)
		{
			ipc::Request message(ipc::RequestType::DeviceManipulation_SetMotionCompensationProperties);
			memset(&message.msg, 0, sizeof(message.msg));
			message.msg.dm_SetMotionCompensationProperties.clientId = m_clientId;
			message.msg.dm_SetMotionCompensationProperties.messageId = 0;
			message.msg.dm_SetMotionCompensationProperties.velAccCompensationModeValid = true;
			message.msg.dm_SetMotionCompensationProperties.velAccCompensationMode = velAccMode;
			message.msg.dm_SetMotionCompensationProperties.kalmanFilterProcessNoiseValid = false;
			message.msg.dm_SetMotionCompensationProperties.kalmanFilterObservationNoiseValid = false;
			if (modal)
			{
				uint32_t messageId = _ipcRandomDist(_ipcRandomDevice);
				message.msg.dm_SetMotionCompensationProperties.messageId = messageId;
				std::promise<ipc::Reply> respPromise;
				auto respFuture = respPromise.get_future();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.insert({ messageId, std::move(respPromise) });
				}
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
				WRITELOG(INFO, "MC message created sending to driver" << std::endl);
				auto resp = respFuture.get();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.erase(messageId);
				}
				std::stringstream ss;
				ss << "Error while setting motion compensation properties: ";
				if (resp.status == ipc::ReplyStatus::InvalidId)
				{
					ss << "Invalid device id";
					throw vrinputemulator_invalidid(ss.str(), (int)resp.status);
				}
				else if (resp.status == ipc::ReplyStatus::NotFound)
				{
					ss << "Device not found";
					throw vrinputemulator_notfound(ss.str(), (int)resp.status);
				}
				else if (resp.status != ipc::ReplyStatus::Ok)
				{
					ss << "Error code " << (int)resp.status;
					throw vrinputemulator_exception(ss.str(), (int)resp.status);
				}
			}
			else
			{
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
				WRITELOG(INFO, "MC message created sending to driver" << std::endl);
			}
		}
		else
		{
			throw vrinputemulator_connectionerror("No active connection.");
		}
	}

	void VRInputEmulator::setMotionCompensationKalmanProcessNoise(double variance, bool modal)
	{
		if (_ipcServerQueue)
		{
			ipc::Request message(ipc::RequestType::DeviceManipulation_SetMotionCompensationProperties);
			memset(&message.msg, 0, sizeof(message.msg));
			message.msg.dm_SetMotionCompensationProperties.clientId = m_clientId;
			message.msg.dm_SetMotionCompensationProperties.messageId = 0;
			message.msg.dm_SetMotionCompensationProperties.velAccCompensationModeValid = false;
			message.msg.dm_SetMotionCompensationProperties.kalmanFilterProcessNoiseValid = true;
			message.msg.dm_SetMotionCompensationProperties.kalmanFilterProcessNoise = variance;
			message.msg.dm_SetMotionCompensationProperties.kalmanFilterObservationNoiseValid = false;
			if (modal)
			{
				uint32_t messageId = _ipcRandomDist(_ipcRandomDevice);
				message.msg.dm_SetMotionCompensationProperties.messageId = messageId;
				std::promise<ipc::Reply> respPromise;
				auto respFuture = respPromise.get_future();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.insert({ messageId, std::move(respPromise) });
				}
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
				WRITELOG(INFO, "MC message created sending to driver" << std::endl);
				auto resp = respFuture.get();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.erase(messageId);
				}
				std::stringstream ss;
				ss << "Error while setting motion compensation properties: ";
				if (resp.status == ipc::ReplyStatus::InvalidId)
				{
					ss << "Invalid device id";
					throw vrinputemulator_invalidid(ss.str(), (int)resp.status);
				}
				else if (resp.status == ipc::ReplyStatus::NotFound)
				{
					ss << "Device not found";
					throw vrinputemulator_notfound(ss.str(), (int)resp.status);
				}
				else if (resp.status != ipc::ReplyStatus::Ok)
				{
					ss << "Error code " << (int)resp.status;
					throw vrinputemulator_exception(ss.str(), (int)resp.status);
				}
			}
			else
			{
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
				WRITELOG(INFO, "MC message created sending to driver" << std::endl);
			}
		}
		else
		{
			throw vrinputemulator_connectionerror("No active connection.");
		}
	}

	void VRInputEmulator::setMotionCompensationKalmanObservationNoise(double variance, bool modal)
	{
		if (_ipcServerQueue)
		{
			ipc::Request message(ipc::RequestType::DeviceManipulation_SetMotionCompensationProperties);
			memset(&message.msg, 0, sizeof(message.msg));
			message.msg.dm_SetMotionCompensationProperties.clientId = m_clientId;
			message.msg.dm_SetMotionCompensationProperties.messageId = 0;
			message.msg.dm_SetMotionCompensationProperties.velAccCompensationModeValid = false;
			message.msg.dm_SetMotionCompensationProperties.kalmanFilterProcessNoiseValid = false;
			message.msg.dm_SetMotionCompensationProperties.kalmanFilterObservationNoiseValid = true;
			message.msg.dm_SetMotionCompensationProperties.kalmanFilterObservationNoise = variance;
			if (modal)
			{
				uint32_t messageId = _ipcRandomDist(_ipcRandomDevice);
				message.msg.dm_SetMotionCompensationProperties.messageId = messageId;
				std::promise<ipc::Reply> respPromise;
				auto respFuture = respPromise.get_future();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.insert({ messageId, std::move(respPromise) });
				}
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
				WRITELOG(INFO, "MC message created sending to driver" << std::endl);
				auto resp = respFuture.get();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.erase(messageId);
				}
				std::stringstream ss;
				ss << "Error while setting motion compensation properties: ";
				if (resp.status == ipc::ReplyStatus::InvalidId)
				{
					ss << "Invalid device id";
					throw vrinputemulator_invalidid(ss.str(), (int)resp.status);
				}
				else if (resp.status == ipc::ReplyStatus::NotFound)
				{
					ss << "Device not found";
					throw vrinputemulator_notfound(ss.str(), (int)resp.status);
				}
				else if (resp.status != ipc::ReplyStatus::Ok)
				{
					ss << "Error code " << (int)resp.status;
					throw vrinputemulator_exception(ss.str(), (int)resp.status);
				}
			}
			else
			{
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
				WRITELOG(INFO, "MC message created sending to driver" << std::endl);
			}
		}
		else
		{
			throw vrinputemulator_connectionerror("No active connection.");
		}
	}

	void VRInputEmulator::setMotionCompensationMovingAverageWindow(unsigned window, bool modal)
	{
		if (_ipcServerQueue)
		{
			ipc::Request message(ipc::RequestType::DeviceManipulation_SetMotionCompensationProperties);
			memset(&message.msg, 0, sizeof(message.msg));
			message.msg.dm_SetMotionCompensationProperties.clientId = m_clientId;
			message.msg.dm_SetMotionCompensationProperties.messageId = 0;
			message.msg.dm_SetMotionCompensationProperties.velAccCompensationModeValid = false;
			message.msg.dm_SetMotionCompensationProperties.kalmanFilterProcessNoiseValid = false;
			message.msg.dm_SetMotionCompensationProperties.kalmanFilterObservationNoiseValid = false;
			message.msg.dm_SetMotionCompensationProperties.movingAverageWindowValid = true;
			message.msg.dm_SetMotionCompensationProperties.movingAverageWindow = window;
			if (modal)
			{
				uint32_t messageId = _ipcRandomDist(_ipcRandomDevice);
				message.msg.dm_SetMotionCompensationProperties.messageId = messageId;
				std::promise<ipc::Reply> respPromise;
				auto respFuture = respPromise.get_future();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.insert({ messageId, std::move(respPromise) });
				}
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
				WRITELOG(INFO, "MC message created sending to driver" << std::endl);
				auto resp = respFuture.get();
				{
					std::lock_guard<std::recursive_mutex> lock(_mutex);
					_ipcPromiseMap.erase(messageId);
				}
				std::stringstream ss;
				ss << "Error while setting motion compensation properties: ";
				if (resp.status == ipc::ReplyStatus::InvalidId)
				{
					ss << "Invalid device id";
					throw vrinputemulator_invalidid(ss.str(), (int)resp.status);
				}
				else if (resp.status == ipc::ReplyStatus::NotFound)
				{
					ss << "Device not found";
					throw vrinputemulator_notfound(ss.str(), (int)resp.status);
				}
				else if (resp.status != ipc::ReplyStatus::Ok)
				{
					ss << "Error code " << (int)resp.status;
					throw vrinputemulator_exception(ss.str(), (int)resp.status);
				}
			}
			else
			{
				_ipcServerQueue->send(&message, sizeof(ipc::Request), 0);
				WRITELOG(INFO, "MC message created sending to driver" << std::endl);
			}
		}
		else
		{
			throw vrinputemulator_connectionerror("No active connection.");
		}
	}


} // end namespace vrinputemulator