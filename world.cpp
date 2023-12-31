#include "core/core_string_names.h"
#include "core/error/error_macros.h"
#include "core/os/memory.h"
#include "core/string/print_string.h"
#include "gdnet.h"
#include "include/steam/isteamnetworkingsockets.h"
#include "include/steam/isteamnetworkingutils.h"
#include <thread>

//===============World Implementation===============//

World::World() {
	m_hListenSock = k_HSteamListenSocket_Invalid;
	m_hPollGroup = k_HSteamNetPollGroup_Invalid;
	m_worldConnection = k_HSteamNetConnection_Invalid;
	m_serverRunLoop = false;
	m_clientRunLoop = false;
}

World::~World() {}

void World::cleanup(){
	//Cleanup the client if there exists a client connetion
	if(GDNet::singleton->m_isClient || m_worldConnection != k_HSteamNetConnection_Invalid){
		leave_world();
	}
}

//=======================================GAMENETWORKINGSOCKETS STUFF - SERVER SIDE=======================================//
void World::SERVER_SIDE_CONN_CHANGE(SteamNetConnectionStatusChangedCallback_t *pInfo) {
	GDNet::singleton->world->SERVER_SIDE_connection_status_changed(pInfo);
}

void World::player_connecting(HSteamNetConnection playerConnection) {
	print_line("Player is connecting...");

	// Make sure the connecting player isnt already connected (or isnt already int he playerconnections map)
	if (m_worldPlayerInfoByConnection.find(playerConnection) != m_worldPlayerInfoByConnection.end()) {
		return;
	}

	// A client is attempting to connect
	// Try to accept the connection
	if (SteamNetworkingSockets()->AcceptConnection(playerConnection) != k_EResultOK) {
		// This could fail.  If the remote host tried to connect, but then
		// disconnected, the connection may already be half closed.  Just
		// destroy whatever we have on our side.
		SteamNetworkingSockets()->CloseConnection(playerConnection, 0, nullptr, false);
		print_line("Can't accept connection.  (It was already closed?)");
		return;
	}

	// Assign the poll group
	if (!SteamNetworkingSockets()->SetConnectionPollGroup(playerConnection, m_hPollGroup)) {
		SteamNetworkingSockets()->CloseConnection(playerConnection, 0, nullptr, false);
		print_line("Failed to set poll group?");
		return;
	}
}

void World::player_connected(HSteamNetConnection playerConnection) {
	//Make sure the connecting player isnt already connected (i have no idea if/why it would here, but just in case :D )
	if (m_worldPlayerInfoByConnection.find(playerConnection) != m_worldPlayerInfoByConnection.end()) {
		return;
	}

	//Allocate player info
	Ref<PlayerInfo> playerInfo(memnew(PlayerInfo));

	//Generate a network identifier for the player
	PlayerID_t playerId = IDGenerator::generatePlayerID();

	//Populate player info
	playerInfo->set_player_conn(playerConnection);
	playerInfo->set_player_id(playerId);

	//Send the player their ID
	SteamNetworkingMessage_t *idAssignmentMssg = create_mini_message(ASSIGN_PLAYER_ID, playerId, playerConnection);
	send_message_reliable(idAssignmentMssg);

	//Add player to a map keyed by connection, and a map keyed by id
	m_worldPlayerInfoByConnection.insert(playerConnection, playerInfo);
	m_worldPlayerInfoById.insert(playerId, playerInfo);

	print_line("Player has connected!");
}

void World::player_disconnected(HSteamNetConnection playerConnection) {
	print_line("Player has disconnected!");
	remove_player(playerConnection);
}

void World::remove_player(HSteamNetConnection hConn) {
	//Remove the player info from the connection keyed map and the id keyed map if they exist
	if (m_worldPlayerInfoByConnection.has(hConn)) {
		//Assign found value to a variable
		Ref<PlayerInfo> info = m_worldPlayerInfoByConnection.get(hConn);
		m_worldPlayerInfoById.erase(info->get_player_id());
		m_worldPlayerInfoByConnection.erase(hConn);
	}

	//Close connection witht the player
	if (!SteamNetworkingSockets()->CloseConnection(hConn, 0, nullptr, false)) {
		print_line("Cannot close connection, it was already closed.");
	}

	print_line("Connection with a player has been closed.");
}


void World::SERVER_SIDE_load_zone_request(const unsigned char *mssgData, HSteamNetConnection sourceConn) {
	print_line("Load Zone Request recieved!");
	// Get the zone requested by the player
	ZoneID_t zoneId = deserialize_mini(mssgData);
	Zone *zone = GDNet::singleton->m_zoneRegistry.get(zoneId).zone;

	// Instantiate the zone if it hasn't been instantiated yet
	if (!zone->m_instantiated) {
		zone->instantiate_zone();
	}

	// Tell the player to load the zone locally on their end
	SteamNetworkingMessage_t *pOutgoingMssg = create_mini_message(LOAD_ZONE_REQUEST, zoneId, sourceConn);
	send_message_reliable(pOutgoingMssg);
}

void World::SERVER_SIDE_load_zone_acknowledge(const unsigned char *mssgData, HSteamNetConnection sourceConn) {
	print_line("Load Zone Ack recieved!");
	// Get the requesting player's id and the zone they requested
	Ref<PlayerInfo> playerInfo = m_worldPlayerInfoByConnection[sourceConn];
	ZoneID_t zoneId = deserialize_mini(mssgData);
	Zone *zone = GDNet::singleton->m_zoneRegistry[zoneId].zone;

	// Add player to the zone's player list (server side)
	zone->add_player(playerInfo);

	// Add zone reference to player info
	playerInfo->set_current_loaded_zone(zone);

	// Make the player start loading all entities in the zone by intitiating the first entity load.
	// This will start a chain of requests that eventually loads all entities on the player's end.
	for(const KeyValue<EntityNetworkID_t, Ref<EntityInfo>> &element : zone->m_entitiesInZone){
		playerInfo->load_entity(element.value);
	}
}

void World::SERVER_SIDE_create_entity_request(const unsigned char *mssgData) {
	print_line("Create entity request recieved!");
	//Create a new entity info refrence to store on the server side
	Ref<EntityInfo> entityInfo(memnew(EntityInfo));

	//Deseralize the message data into the reference
	entityInfo->deserialize_info(mssgData);

	//Assign a network id for the entity
	entityInfo->m_entityInfo.networkId = IDGenerator::generateNetworkIdentityID();

	//==Instantiate the network entity at the specified path relative to the zone==//
	//Store local references to relevant objects
	EntityID_t entityId = entityInfo->get_entity_id();
	ZoneID_t parentZoneId = entityInfo->m_entityInfo.parentZone;
	Zone* parentZone = GDNet::singleton->m_zoneRegistry[parentZoneId].zone;
	String parentRelativePath = entityInfo->get_parent_relative_path();

	//Instantiate the entity
	Node* instance = GDNet::singleton->m_networkEntityRegistry[entityId].scene->instantiate();
	entityInfo->m_entityInfo.entityInstance = Object::cast_to<NetworkEntity>(instance);

	//Get a refrence to the requested parent node if one was provided. Otherwise just use the instance as the base node.
	Node *parentNode;
	if(parentRelativePath == ""){
		parentNode = parentZone->m_zoneInstance;
	}else {
	    parentNode = parentZone->m_zoneInstance->get_node(parentRelativePath);
	}

	//Add the entity to the zone scene
	parentNode->call_deferred("add_child", instance);

	//Add the entity to list of known entities in zone
	parentZone->m_entitiesInZone.insert(entityInfo->m_entityInfo.networkId, entityInfo);

	//Associate the entity with a player (if such a player was specified)
	PlayerID_t associatedPlayer = entityInfo->get_associated_player_id();
	if(associatedPlayer != 0){
		m_worldPlayerInfoById.get(associatedPlayer)->add_associated_entity(entityInfo);
	}

	//Reserialize the entity info with the new data in it
	entityInfo->serialize_info();

	//Tell each player in the zone to also create this entity
	for (const Ref<PlayerInfo> &player : parentZone->m_playersInZone) {
		player->load_entity(entityInfo);
	}
}

void World::SERVER_SIDE_create_entity_acknowledge(const unsigned char *mssgData, HSteamNetConnection sourceConn) {
	//Get the player who sent the acknowledgement
	Ref<PlayerInfo> playerInfo = m_worldPlayerInfoByConnection.get(sourceConn);

	//Deserialize the acknowledgement message
	EntityNetworkID_t networkIdAck = deserialize_mini(mssgData);

	//Confirm by recording that the entity has been created successfully on the client's side
	playerInfo->confirm_entity(networkIdAck);
}


void World::SERVER_SIDE_connection_status_changed(SteamNetConnectionStatusChangedCallback_t *pInfo) {
	//What is the state of the connection?
	switch (pInfo->m_info.m_eState) {
		case k_ESteamNetworkingConnectionState_None:
			//NOTE: Callbacks will be sent here when connections are destoryed. These can be ignored
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer: {
			player_disconnected(pInfo->m_hConn);
			break;
		}

		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			print_line("Player connection has dropped improperly!");
			break;

		case k_ESteamNetworkingConnectionState_Connecting:
			player_connecting(pInfo->m_hConn);
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			// We will get a callback immediately after accepting the connection.
			player_connected(pInfo->m_hConn);
			break;

		default:
			//No mans land
			//God knows what the hell would end up here. Hopefully nothing important :3
			break;
	}
}

void World::SERVER_SIDE_poll_incoming_messages() {
	while (m_serverRunLoop) {
		SteamNetworkingMessage_t *pIncomingMsgs[16];
		int numMsgs = SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(m_hPollGroup, pIncomingMsgs, 16);

		if (numMsgs == 0) {
			break;
		}

		if (numMsgs < 0) {
			ERR_PRINT("Error checking messages");
			m_serverRunLoop = false;
			return;
		}

		//Evaluate each message
		for (int i = 0; i < numMsgs; i++) {
			SteamNetworkingMessage_t *pMessage = pIncomingMsgs[i];
			const unsigned char *mssgData = static_cast<unsigned char *>(pMessage->m_pData);

			//Check the type of message recieved
			switch (mssgData[0]) {
				case LOAD_ZONE_REQUEST:
					SERVER_SIDE_load_zone_request(mssgData, pMessage->m_conn);
					break;
				case LOAD_ZONE_ACKNOWLEDGE:
					SERVER_SIDE_load_zone_acknowledge(mssgData, pMessage->m_conn);
					break;
				case CREATE_ENTITY_REQUEST:
					SERVER_SIDE_create_entity_request(mssgData);
					break;
				case CREATE_ENTITY_ACKNOWLEDGE:
					SERVER_SIDE_create_entity_acknowledge(mssgData, pMessage->m_conn);
					break;
				default:
					break;
			}

			//Dispose of the message
			pMessage->Release();
		}
	}
}

void World::server_listen_loop() {
	while (m_serverRunLoop) {
		SERVER_SIDE_poll_incoming_messages();
		SteamNetworkingSockets()->RunCallbacks();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}
//=======================================================================================================================//

//=======================================GAMENETWORKINGSOCKETS STUFF - CLIENT SIDE=======================================//
void World::CLIENT_SIDE_CONN_CHANGE(SteamNetConnectionStatusChangedCallback_t *pInfo) {
	GDNet::singleton->world->CLIENT_SIDE_connection_status_changed(pInfo);
}


void World::CLIENT_SIDE_player_entered_zone(const unsigned char *mssgData) {
	PlayerID_t playerId = deserialize_mini(mssgData);
}

void World::CLIENT_SIDE_process_create_entity_request(const unsigned char *mssgData) {
	print_line("Create entity request recieved!");
	//Create a new entity info refrence to store on the client side
	Ref<EntityInfo> entityInfo(memnew(EntityInfo));

	//Deseralize the message data into the reference
	entityInfo->deserialize_info(mssgData);

	//==Instantiate the network entity at the specified path relative to the zone==//
	//Store local references to relevant objects
	EntityID_t entityId = entityInfo->get_entity_id();
	ZoneID_t parentZoneId = entityInfo->m_entityInfo.parentZone;
	Zone* parentZone = GDNet::singleton->m_zoneRegistry[parentZoneId].zone;
	String parentRelativePath = entityInfo->get_parent_relative_path();

	//Instantiate the entity
	Node* instance = GDNet::singleton->m_networkEntityRegistry[entityId].scene->instantiate();
	entityInfo->m_entityInfo.entityInstance = Object::cast_to<NetworkEntity>(instance);

	//Get a refrence to the requested parent node if one was provided. Otherwise just use the instance as the base node.
	Node *parentNode;
	if(parentRelativePath == ""){
		parentNode = parentZone->m_zoneInstance;
	}else {
		parentNode = parentZone->m_zoneInstance->get_node(parentRelativePath);
	}

	//Add the entity to the zone scene
	parentNode->call_deferred("add_child", instance);

	//Add the entity to list of known entities in zone
	parentZone->m_entitiesInZone[entityInfo->m_entityInfo.networkId] = entityInfo;

	//Send entity creation acknowledgement to server
	EntityNetworkID_t networkId = entityInfo->m_entityInfo.networkId;
	HSteamNetConnection worldConn = GDNet::singleton->world->m_worldConnection;

	SteamNetworkingMessage_t* ackMssg = create_mini_message(CREATE_ENTITY_ACKNOWLEDGE, networkId, worldConn);
	send_message_reliable(ackMssg);
}

void World::CLIENT_SIDE_load_zone_request(const unsigned char *mssgData) {
	//Get the zone id that the server wants instantiated
	ZoneID_t zoneId = deserialize_mini(mssgData);
	print_line(vformat("Loading zone with id %d", zoneId));

	//If instantiation was successful:
	if(instantiate_zone_by_id(zoneId)){
		SteamNetworkingMessage_t *zoneLoadAck = create_mini_message(LOAD_ZONE_ACKNOWLEDGE, zoneId, m_worldConnection);
		send_message_reliable(zoneLoadAck);
	}
}

void World::CLIENT_SIDE_assign_player_id(const unsigned char *mssgData) {
	PlayerID_t id = deserialize_mini(mssgData);
	print_line(vformat("Assigned player id is %d", id));

	//Since this entire loop is running in a different thread from the main thread/game loop,
	//the signal has to be queued to be emitted at the next game loop call.
	call_deferred("emit_signal", "joined_world");
}


void World::CLIENT_SIDE_connection_status_changed(SteamNetConnectionStatusChangedCallback_t *pInfo) {
	//What's the state of the connection?
	switch (pInfo->m_info.m_eState) {
		case k_ESteamNetworkingConnectionState_None:
			// NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			break;
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
			m_clientRunLoop = false;

			// Print an appropriate message
			if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting) {
				// Note: we could distinguish between a timeout, a rejected connection,
				// or some other transport problem.
				print_line("We sought the remote host, yet our efforts were met with defeat. ", pInfo->m_info.m_szEndDebug);
			} else if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
				print_line("Alas, troubles beset us; we have lost contact with the host. ", pInfo->m_info.m_szEndDebug);
			} else {
				// NOTE: We could check the reason code for a normal disconnection
				print_line("The host hath bidden us farewell.  (%s)", pInfo->m_info.m_szEndDebug);
			}

			// Clean up the connection.  This is important!
			// The connection is "closed" in the network sense, but
			// it has not been destroyed.  We must close it on our end, too
			// to finish up.  The reason information do not matter in this case,
			// and we cannot linger because it's already closed on the other end,
			// so we just pass 0's.
			SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
			m_worldConnection = k_HSteamNetConnection_Invalid;
			break;
		}

		case k_ESteamNetworkingConnectionState_Connecting:
			// We will get this callback when we start connecting.
			print_line("Connecting to world...");
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			print_line("Successfully connected to world!");
			break;

		default:
			// Silences -Wswitch
			break;
	}
}

void World::CLIENT_SIDE_poll_incoming_messages() {
	while (m_clientRunLoop) {
		SteamNetworkingMessage_t *pIncomingMsgs[1];
		int numMsgs = SteamNetworkingSockets()->ReceiveMessagesOnConnection(m_worldConnection, pIncomingMsgs, 1);

		if (numMsgs == 0) {
			return;
		}

		if (numMsgs < 0) {
			ERR_PRINT("Error checking messages");
			m_clientRunLoop = false;
			return;
		}

		//Evaluate each message
		for (int i = 0; i < numMsgs; i++) {
			SteamNetworkingMessage_t *pMessage = pIncomingMsgs[i];
			const unsigned char *mssgData = static_cast<unsigned char *>(pMessage->m_pData);

			//Check the type of message recieved and evaluate accordingly
			switch (mssgData[0]) {
				case ASSIGN_PLAYER_ID:
					CLIENT_SIDE_assign_player_id(mssgData);
					break;
				case LOAD_ZONE_REQUEST:
					CLIENT_SIDE_load_zone_request(mssgData);
					break;
				case PLAYER_ENTERED_ZONE:
					CLIENT_SIDE_player_entered_zone(mssgData);
					break;
				case CREATE_ENTITY_REQUEST:
					CLIENT_SIDE_process_create_entity_request(mssgData);
					break;
				default:
					break;
			}

			//Dispose of the message
			pMessage->Release();
		}
	}
}

void World::client_listen_loop() {
	while (m_clientRunLoop) {
		CLIENT_SIDE_poll_incoming_messages();
		SteamNetworkingSockets()->RunCallbacks();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

//=======================================================================================================================//

//==Protected Mehtods==//

void World::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_world", "port"), &World::start_world);
	ClassDB::bind_method(D_METHOD("stop_world"), &World::stop_world);
	ClassDB::bind_method(D_METHOD("join_world", "world", "port"), &World::join_world);
	ClassDB::bind_method(D_METHOD("leave_world"), &World::leave_world);
	ClassDB::bind_method(D_METHOD("load_zone_by_name", "zone_name"), &World::load_zone_by_name);
	ClassDB::bind_method(D_METHOD("load_zone_by_id", "zone_id"), &World::load_zone_by_id);

	ADD_SIGNAL(MethodInfo("joined_world"));
}

//==Public Methods==//

bool World::instantiate_zone_by_id(ZoneID_t zoneId) {
	if (GDNet::singleton->m_zoneRegistry.has(zoneId)) {
		GDNet::singleton->m_zoneRegistry.get(zoneId).zone->instantiate_zone();
		return true;
	} else {
		return false;
	}
}

void World::start_world(int port) {
	if (!GDNet::singleton->m_isInitialized) {
		ERR_PRINT("GDNet is not initialized!");
		return;
	}

	//Define network connection info
	SteamNetworkingIPAddr worldInfo{};
	worldInfo.Clear();
	worldInfo.m_port = (uint16)port;

	//Create a poll group
	m_hPollGroup = SteamNetworkingSockets()->CreatePollGroup();

	//Server Config (set callbacks)
	SteamNetworkingConfigValue_t cfg{};
	cfg.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void *)World::SERVER_SIDE_CONN_CHANGE);

	//Create a listening socket with the server config
	m_hListenSock = SteamNetworkingSockets()->CreateListenSocketIP(worldInfo, 1, &cfg);

	//Make sure the listening socket is valid, otherwise report an error and terminate start procedure.
	if (m_hListenSock == k_HSteamListenSocket_Invalid) {
		ERR_FAIL_MSG(vformat("Failed to listen on port %d (invalid socket)", port));
		return;
	}

	//Make sure the poll group is valid, otherwirse report and error and terminate the start prcedure.
	if (m_hPollGroup == k_HSteamNetPollGroup_Invalid) {
		ERR_FAIL_MSG(vformat("Failed to listen on port %d (invalid poll group)", port));
		return;
	}

	//Start the main server loop
	m_serverRunLoop = true;
	m_serverListenThread = std::thread(&World::server_listen_loop, this);

	//Indicate that the world is acting as a server, not a client
	GDNet::singleton->m_isServer = true;

	//TEMP: confirm that the server has started on the requested port:
	print_line(vformat("Server has started on port %d!", port));
}

void World::stop_world() {
	m_serverRunLoop = false;

	if (m_serverListenThread.joinable()) {
		m_serverListenThread.join();
	}

	//Close the socket
	SteamNetworkingSockets()->CloseListenSocket(m_hListenSock);
	m_hListenSock = k_HSteamListenSocket_Invalid;

	//Destroy poll group
	SteamNetworkingSockets()->DestroyPollGroup(m_hPollGroup);
	m_hPollGroup = k_HSteamNetPollGroup_Invalid;

	//Indicate that the world is no longer acting as the server
	GDNet::singleton->m_isServer = false;
}

void World::join_world(String world, int port) {
	//Define network connection info
	SteamNetworkingIPAddr worldInfo{};
	worldInfo.Clear();
	worldInfo.ParseString(world.utf8().get_data());
	worldInfo.m_port = port;

	//Client config (set callbacks)
	SteamNetworkingConfigValue_t cfg{};
	cfg.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void *)World::CLIENT_SIDE_CONN_CHANGE);

	//Connect to world
	m_worldConnection = SteamNetworkingSockets()->ConnectByIPAddress(worldInfo, 1, &cfg);

	//Make sure the player was able to connect to the world
	if (m_worldConnection == k_HSteamNetConnection_Invalid) {
		ERR_PRINT("Unable to conncect to world.");
		return;
	}

	//Start the client listen loop
	m_clientRunLoop = true;
	m_clientListenThread = std::thread(&World::client_listen_loop, this);

	GDNet::singleton->m_isClient = true;
	print_line("Connection to world has initiated.");
}

void World::leave_world() {
	//Make sure client is connected to a world
	if (!GDNet::singleton->m_isClient || m_worldConnection == k_HSteamNetConnection_Invalid){
		ERR_PRINT("Not connected to a world, therefore cannot leave world!");
		return;
	}

	m_clientRunLoop = false;

	//Stop the run loop
	if (m_clientListenThread.joinable()){
		m_clientListenThread.join();
	}

	//Stop world connection
	SteamNetworkingSockets()->CloseConnection(m_worldConnection, 0, nullptr, false);
	m_worldConnection = k_HSteamNetConnection_Invalid;

	GDNet::singleton->m_isClient = false;
}

bool World::load_zone_by_name(String zoneName) {
	if (GDNet::singleton->m_isServer) {
		print_line("Cannot request to load zone as the world host!");
		return false;
	}

	print_line("Finding Zone...");

	for (const KeyValue<ZoneID_t, ZoneInfo> &element : GDNet::singleton->m_zoneRegistry) {
		if (element.value.name == zoneName) {
			print_line("Zone Found! sending load zone request.");
			ZoneID_t zoneId = element.key;
			SteamNetworkingMessage_t *pLoadZoneRequest = create_mini_message(LOAD_ZONE_REQUEST, zoneId, m_worldConnection);
			send_message_reliable(pLoadZoneRequest);
			return true;
		}
	}

	print_line("Could not find zone :(");
	return false;
}

bool World::load_zone_by_id(ZoneID_t zoneId) {
	if (GDNet::singleton->m_isServer) {
		print_line("Cannot request to load zone as the world host!");
		return false;
	}

	if (GDNet::singleton->m_zoneRegistry.has(zoneId)) {
		print_line("Zone Found! sending load zone request.");
		SteamNetworkingMessage_t *pLoadZoneRequest = create_mini_message(LOAD_ZONE_REQUEST, zoneId, m_worldConnection);
		send_message_reliable(pLoadZoneRequest);
		return true;
	}else{
		print_line("Zone not found :(");
		return false;
	}
}

bool World::player_exists(PlayerID_t playerId) {
	if(!GDNet::singleton->m_isClient && !GDNet::singleton->m_isServer){
		ERR_PRINT("Cannot lookup players since there is no world running and there is no connection to a world.");
		return false;
	}

	return m_worldPlayerInfoById.find(playerId) != m_worldPlayerInfoById.end();
}
