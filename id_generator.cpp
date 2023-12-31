#include "gdnet.h"

// Assign static counters their initial values.
unsigned int IDGenerator::s_playerIDCounter = 1;
unsigned int IDGenerator::s_networkEntityIDCounter = 1;

std::stack<unsigned int> IDGenerator::freePlayerIDs;
std::stack<unsigned int> IDGenerator::freeNetworkEntityIDs;
std::unordered_set<unsigned int> IDGenerator::usedPlayerIDs;
std::unordered_set<unsigned int> IDGenerator::usedNetworkEntityIDs;

PlayerID_t IDGenerator::generatePlayerID() {
	unsigned int newID;

	if (!freePlayerIDs.empty()) {
		// If there exists a free player id, use the first one on the stack as the new id
		newID = freePlayerIDs.top();
		freePlayerIDs.pop();
	} else {
		// Keep incrementing the id counter until a suitible unused int is found. Add it to
		// the used list and return it.
		do {
			newID = s_playerIDCounter++;
		} while (usedPlayerIDs.find(newID) != usedPlayerIDs.end());
	}

	usedPlayerIDs.insert(newID);
	return newID;
}

EntityNetworkID_t IDGenerator::generateNetworkIdentityID() {
	unsigned int newID;

	if (!freeNetworkEntityIDs.empty()) {
		// If there exists a free network entity id, use the first one on the stack as the new id
		newID = freeNetworkEntityIDs.top();
		freeNetworkEntityIDs.pop();
	} else {
		// Keep incrementing the id counter until a suitible unused int is found. Add it to
		// the used list and return it.
		do {
			newID = s_networkEntityIDCounter++;
		} while (usedNetworkEntityIDs.find(newID) != usedNetworkEntityIDs.end());
	}

	usedNetworkEntityIDs.insert(newID);
	return newID;
}

void IDGenerator::freePlayerID(PlayerID_t playerID) {
	// Move the id from used to free.
	usedPlayerIDs.erase(playerID);
	freePlayerIDs.push(playerID);
}

void IDGenerator::freeNetworkEntityID(EntityNetworkID_t networkEntityID) {
	//Move the id from used to free.
	usedNetworkEntityIDs.erase(networkEntityID);
	freeNetworkEntityIDs.push(networkEntityID);
}
