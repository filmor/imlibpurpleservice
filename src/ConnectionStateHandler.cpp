/*
 * ConnectionStateHandler.cpp
 *
 * Copyright 2010 Palm, Inc. All rights reserved.
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free
 * Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License,
 * Version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-
 * 1301, USA
 *
 * IMLibpurpleservice uses libpurple.so to implement a fully functional IM
 * Transport service for use on a mobile device.
 *
 * ConnectionStateHandler class handles changes between WAN and WiFi device connection
 */

#include "ConnectionStateHandler.h"
#include "IMServiceApp.h"
#include "IMServiceHandler.h"

ConnectionState* ConnectionState::m_connState = NULL;
IMLoginState* ConnectionState::m_loginState = NULL;

ConnectionState::ConnectionState(MojService* service)
: m_inetConnected(false),
  m_wanConnected(false),
  m_wifiConnected(false),
  m_service(service)
{
	assert(ConnectionState::m_connState == NULL);
	ConnectionState::m_connState = this;
	m_handler = new ConnectionState::ConnectionStateHandler(service, this);
}

bool ConnectionState::hasInternetConnection() {
	MojLogInfo(IMServiceApp::s_log, _T("!!!!!!!!!wifi=%i, wan=%i, inet=%i"), ConnectionState::m_connState->m_wifiConnected, ConnectionState::m_connState->m_wanConnected,
			ConnectionState::m_connState->m_inetConnected);
	// we should look at m_inetConnected too... there are cases where connection manager says wan is connected but "unuseable" if it connection has not been torn down yet, but isInternetAvailable is false here
	// {"internet":{"bridge":{"state":"disconnected"},"isInternetConnectionAvailable":false,"vpn":{"state":"disconnected"},
	//    "wan":{"interfaceName":"wwan0","ipAddress":"10.13.118.115","network":"unusable","networkConfidenceLevel":"none","onInternet":"yes","state":"connected"},
	//    "wifi":{"state":"disconnected"}}}}
	return (ConnectionState::m_connState->m_inetConnected == true && (ConnectionState::m_connState->m_wifiConnected == true || ConnectionState::m_connState->m_wanConnected == true));
}

// This provides connectivity details when IMTransport has first launched and the
// initial response from our ConnectionManager subscription hasn't come back.
void ConnectionState::initConnectionStatesFromActivity(const MojObject& activity)
{
	// log the parameters
	//IMServiceHandler::logMojObjectJsonString(_T("ConnectionState::initConnectionStatesFromActivity: %s"), activity);


	//{"$activity":{"activityId":16,"requirements":{"internet":{"btpan":{"state":"disconnected"},"isInternetConnectionAvailable":true,"wan":{"ipAddress":"10.13.230.47","network":"hsdpa","state":"connected"},"wifi":{"bssid":"00:17:DF:AB:49:70","ipAddress":"10.92.0.177","ssid":"Guest","state":"connected"}}},"trigger":{"fired":true,"returnValue":true}}}
	if (m_handler && m_handler->hasConnData() == false)
	{
		MojLogInfo(IMServiceApp::s_log, _T("initConnectionStatesFromActivity: ConnectionStateHandler needs data"));

		// activity is a const so we have to copy it to pass to slot handler...
		MojObject activityObj(activity);
		m_handler->connectionManagerResult(activityObj, MojErrNone);

		// TODO - why is this code here?? It does not actually work since connectionManagerResult expects an $activity object...
		// seems we get the connection manager response via activity manager immediately after, so this is not really needed...

//		MojObject activityObj;
//		bool found = activity.get("$activity", activityObj);
//		if (found)
//		{
//			MojObject requirements;
//			found = activity.get("requirements", requirements);
//			if (found)
//			{
//				MojObject internet;
//				found = requirements.get("internet", internet);
//				if (found && m_handler)
//				{
//					m_handler->connectionManagerResult(internet, MojErrNone);
//				}
//			}
//		}
	}
}

void ConnectionState::getBestConnection(MojString& connectionType, MojString& ipAddress)
{
	if (ConnectionState::m_connState->m_wifiConnected)
	{
		connectionType.assign("wifi");
		ipAddress = ConnectionState::m_connState->m_wifiIpAddress;
	}
	else
	{
		connectionType.assign("wan");
		ipAddress = ConnectionState::m_connState->m_wanIpAddress;
	}
}

void ConnectionState::connectHandlerDied()
{
	m_handler = NULL;
	//TODO: restart the connection manager subscription
}



ConnectionState::ConnectionStateHandler::ConnectionStateHandler(MojService* service, ConnectionState* connState)
: m_connMgrSubscriptionSlot(this, &ConnectionState::ConnectionStateHandler::connectionManagerResult),
  m_service(service),
  m_connState(connState),
  m_receivedResponse(false)
{
	MojLogInfo(IMServiceApp::s_log, "ConnectionStateHandler constructor");

	MojRefCountedPtr<MojServiceRequest> req;
	MojErr err = m_service->createRequest(req);
	if (err)
	{
		MojLogError(IMServiceApp::s_log, _T("ConnectionStateHandler: create request failed"));
	}
	else
	{
		MojObject params;
		params.put("subscribe", true);
		params.put("start", true);
		params.put("detailedEvents", true); // This tells activitymanager to send us updates whenever the trigger changes

		const char* activityJson =
				"{\"name\":\"Libpurple connection watch\","
				"\"description\":\"Libpurple connection watch\","
				// Need immediate:true to prevent blocking other low priority activities from running
				"\"type\": {\"immediate\": true, \"priority\": \"low\"},"
				"\"requirements\":{\"internet\":true}"
				"}";
		MojObject activity;
		activity.fromJson(activityJson);
		params.put("activity", activity);

		err = req->send(m_connMgrSubscriptionSlot, "com.palm.activitymanager","create", params, MojServiceRequest::Unlimited);
		if (err)
		{
			MojLogError(IMServiceApp::s_log, _T("ConnectionStateHandler send request failed"));
		}
	}
}


ConnectionState::ConnectionStateHandler::~ConnectionStateHandler()
{
	MojLogInfo(IMServiceApp::s_log, "ConnectionStateHandler destructor");
	m_connState->connectHandlerDied();
}

//TODO: when the connection is lost (no internet), complete/cancel the activity and exit the service
MojErr ConnectionState::ConnectionStateHandler::connectionManagerResult(MojObject& result, MojErr err)
{
	// log the parameters
	IMServiceHandler::logMojObjectJsonString(_T("ConnectionStateHandler::connectionManagerResult: %s"), result);

	if (err == MojErrNone && result.contains("$activity"))
	{
		MojObject activity, requirements, internetRequirements;
		// The connectionmanager status is under $activity.requirements.internet
		err = result.getRequired("$activity", activity);
		if (err == MojErrNone)
		{
			err = activity.getRequired("requirements", requirements);
			if (err == MojErrNone)
			{
				err = requirements.getRequired("internet", internetRequirements);
			}
		}

		if (err == MojErrNone)
		{
			m_receivedResponse = true;
			bool prevInetConnected = m_connState->m_inetConnected;
			internetRequirements.get("isInternetConnectionAvailable", m_connState->m_inetConnected);

			bool prevWifiConnected = m_connState->m_wifiConnected;
			bool found = false;
			MojObject wifiObj;
			err = internetRequirements.getRequired("wifi", wifiObj);
			MojString wifiState;
			err = wifiObj.getRequired("state", wifiState);
			m_connState->m_wifiConnected = (err == MojErrNone && wifiState.compare("connected") == 0);
			// If the connection was lost, keep the old ipAddress so it can be used to
			// notify accounts that were using it
			if (m_connState->m_wifiConnected == true)
			{
				err = wifiObj.get("ipAddress", m_connState->m_wifiIpAddress, found);
				if (err != MojErrNone || found == false)
				{
					// It may claim to be connected, but there's no interface IP address
					MojLogError(IMServiceApp::s_log, _T("Marking WiFi as disconnected because ipAddress is missing"));
					m_connState->m_wifiConnected = false;
				}
				MojLogInfo(IMServiceApp::s_log, _T("ConnectionStateHandler::connectionManagerResult found=%i, wifi ipAddress=%s"), found, m_connState->m_wifiIpAddress.data());
			}

			bool prevWanConnected = m_connState->m_wanConnected;

			MojObject wanObj;
			err = internetRequirements.getRequired("wan", wanObj);
			MojErrCheck(err);

			MojString wanState;
			err = wanObj.getRequired("state", wanState);
			MojErrCheck(err);

			m_connState->m_wanConnected = (err == MojErrNone && wanState.compare("connected") == 0);
			// If the connection was lost, keep the old ipAddress so it can be used to
			// notify accounts that were using it
			if (m_connState->m_wanConnected == true)
			{
				err = wanObj.get("ipAddress", m_connState->m_wanIpAddress, found);
				if (err != MojErrNone || found == false)
				{
					// It may claim to be connected, but there's no interface IP address
					MojLogError(IMServiceApp::s_log, _T("Marking WAN as disconnected because ipAddress is missing"));
					m_connState->m_wanConnected = false;
				}
				MojLogInfo(IMServiceApp::s_log, _T("ConnectionStateHandler::connectionManagerResult found=%i, wan ipAddress=%s"), found, m_connState->m_wanIpAddress.data());
			}

			// If the loginState machine setup a listener, then post the change
			if (m_loginState != NULL)
			{
				if (prevInetConnected != m_connState->m_inetConnected ||
					prevWifiConnected != m_connState->m_wifiConnected ||
					prevWanConnected != m_connState->m_wanConnected)
				{
					MojLogInfo(IMServiceApp::s_log, _T("ConnectionStateHandler::connectionManagerResult - connection changed - scheduling activity."));
					ConnectionState::ConnectionChangedScheduler *changeScheduler = new ConnectionState::ConnectionChangedScheduler(m_service);
					changeScheduler->scheduleActivity();
				}
				else {
					MojLogInfo(IMServiceApp::s_log, _T("ConnectionStateHandler::connectionManagerResult - no change from previous state."));
				}
			}
		}
	}
	else {
		MojLogInfo(IMServiceApp::s_log, _T("ConnectionStateHandler::connectionManagerResult no activity object - ignoring. err = %d"), err);
	}
	return MojErrNone;
}


ConnectionState::ConnectionChangedScheduler::ConnectionChangedScheduler(MojService* service)
: m_scheduleActivitySlot(this, &ConnectionState::ConnectionChangedScheduler::scheduleActivityResult),
  m_activityCompleteSlot(this, &ConnectionState::ConnectionChangedScheduler::activityCompleteResult),
  m_service(service)
{
}

MojErr ConnectionState::ConnectionChangedScheduler::scheduleActivity()
{
	MojLogInfo(IMServiceApp::s_log, _T("com.palm.activitymanager/create \"IMLibpurple Connect Changed\""));
	MojRefCountedPtr<MojServiceRequest> req;
	MojErr err = m_service->createRequest(req);
	if (err != MojErrNone)
	{
		MojLogError(IMServiceApp::s_log, _T("ConnectionChangedScheduler createRequest failed for connect debounce. error %d"), err);
	}
	else
	{
		// Schedule an activity to callback after a few seconds. The activity may already exist
		MojObject activity;
		const MojChar* activityJSON = _T("{\"name\":\"IMLibpurple Connect Changed\"," \
				"\"description\":\"Scheduled callback to check the current connection status\"," \
				"\"type\":{\"foreground\":true}}");
		activity.fromJson(activityJSON);
		// activity.schedule
		time_t targetDate;
		time(&targetDate);
		targetDate += 10; // 10 seconds in the future
		tm* ptm = gmtime(&targetDate);
		char scheduleTime[50];
		sprintf(scheduleTime, "%d-%02d-%02d %02d:%02d:%02dZ", ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday, ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
		MojObject scheduleObj;
		scheduleObj.putString("start", scheduleTime);
		activity.put("schedule", scheduleObj);

		MojObject params;
		params.putBool("start", true);
		params.putBool("subscribe", true);
		params.put("activity", activity);
		err = req->send(m_scheduleActivitySlot, "com.palm.activitymanager", "create", params, MojServiceRequest::Unlimited);
	}
	return err;
}

// Expected responses
// {"activityId":78,"returnValue":true}
// {"activityId":78,"event":"start","returnValue":true}  <-- this occurs when the timeout fires
// Error in case there is already an activity for the given serviceName
// {"errorCode":17,"errorText":"Activity with that name already exists","returnValue":false}
MojErr ConnectionState::ConnectionChangedScheduler::scheduleActivityResult(MojObject& result, MojErr err)
{
	// log the parameters
	IMServiceHandler::logMojObjectJsonString(_T("ConnectionChangedScheduler::scheduleActivityResult: %s"), result);

	if (MojErrNone != err) {
		MojString error;
		MojErrToString(err, error);
		MojLogError(IMServiceApp::s_log, _T("ConnectionChangedScheduler::scheduleActivityResult failed: error %d - %s"), err, error.data());
		// we created this slot with unlimited responses, so we have to explicitly cancel it, unless there is another activity of this name already, in which case
		// we get here again
		if (MojErrExists != err) { // err 17 = "Activity with that name already exists"
			m_scheduleActivitySlot.cancel();
		}
	}
	else {
		MojString event;
		if (result.getRequired(_T("event"), event) == MojErrNone)
		{
			MojLogInfo(IMServiceApp::s_log, _T("ConnectionChangedScheduler::scheduleActivityResult event=%s"), event.data());
			if (event == "start")
			{
				MojInt64 activityId = 0;
				MojObject dummy;
				m_loginState->handleConnectionChanged(dummy);

				bool found = result.get("activityId", activityId);
				if (found && activityId > 0)
				{
					MojRefCountedPtr<MojServiceRequest> req;
					err = m_service->createRequest(req);
					MojObject completeParams;
					completeParams.put(_T("activityId"), activityId);
					err = req->send(m_activityCompleteSlot, "com.palm.activitymanager", "complete", completeParams, 1);
				}
				else {
					MojLogError(IMServiceApp::s_log,_T("ConnectionChangedScheduler::scheduleActivityResult - missing activityId"));
				}
				if (activityId <= 0 || err) {
					MojString error;
					MojErrToString(err, error);
					MojLogError(IMServiceApp::s_log, _T("ConnectionChangedScheduler::scheduleActivityResult send request failed: error %d - %s"), err, error.data());
					// we created this slot with unlimited responses, so we have to explicitly cancel
					m_scheduleActivitySlot.cancel();
				}
			}
		}
		// if there is no event we get here again
	}

	return MojErrNone;
}

MojErr ConnectionState::ConnectionChangedScheduler::activityCompleteResult(MojObject& result, MojErr err)
{
	m_scheduleActivitySlot.cancel();
	return err;
}

