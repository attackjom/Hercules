/**
 * This file is part of Hercules.
 * http://herc.ws - http://github.com/HerculesWS/Hercules
 *
 * Copyright (C) 2012-2020 Hercules Dev Team
 * Copyright (C) Athena Dev Teams
 *
 * Hercules is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define HERCULES_CORE

#include "int_party.h"

#include "char/char.h"
#include "char/inter.h"
#include "char/mapif.h"
#include "common/cbasetypes.h"
#include "common/db.h"
#include "common/memmgr.h"
#include "common/mapindex.h"
#include "common/mmo.h"
#include "common/nullpo.h"
#include "common/showmsg.h"
#include "common/socket.h"
#include "common/sql.h"
#include "common/strlib.h"

#include <stdio.h>
#include <stdlib.h>

static struct inter_party_interface inter_party_s;
struct inter_party_interface *inter_party;

//Updates party's level range and unsets even share if broken.
static int inter_party_check_lv(struct party_data *p)
{
	int i;
	unsigned int lv;
	nullpo_ret(p);
	p->min_lv = UINT_MAX;
	p->max_lv = 0;
	for(i=0;i<MAX_PARTY;i++){
		/**
		 * - If not online OR if it's a family party and this is the child (doesn't affect exp range)
		 **/
		if(!p->party.member[i].online || p->party.member[i].char_id == p->family )
			continue;

		lv=p->party.member[i].lv;
		if (lv < p->min_lv) p->min_lv = lv;
		if (lv > p->max_lv) p->max_lv = lv;
	}

	if (p->party.exp && !inter_party->check_exp_share(p)) {
		p->party.exp = 0;
		mapif->party_optionchanged(0, &p->party, 0, 0);
		return 0;
	}
	return 1;
}
//Calculates the state of a party.
static void inter_party_calc_state(struct party_data *p)
{
	int i;
	nullpo_retv(p);
	p->min_lv = UINT_MAX;
	p->max_lv = 0;
	p->party.count =
	p->size =
	p->family = 0;

	//Check party size
	for(i=0;i<MAX_PARTY;i++){
		if (!p->party.member[i].lv) continue;
		p->size++;
		if(p->party.member[i].online)
			p->party.count++;
	}
	// FIXME[Haru]: What if the occupied positions aren't the first three? It can happen if some party members leave. This is the reason why family sharing some times stops working until you recreate your party
	if( p->size == 2 && ( chr->char_child(p->party.member[0].char_id,p->party.member[1].char_id) || chr->char_child(p->party.member[1].char_id,p->party.member[0].char_id) ) ) {
		//Child should be able to share with either of their parents  [RoM]
		if (p->party.member[0].class >= JOB_BABY && p->party.member[0].class <= JOB_SUPER_BABY) //first slot is the child?
			p->family = p->party.member[0].char_id;
		else
			p->family = p->party.member[1].char_id;
	} else if( p->size == 3 ) {
		//Check Family State.
		p->family = chr->char_family(
			p->party.member[0].char_id,
			p->party.member[1].char_id,
			p->party.member[2].char_id
		);
	}
	//max/min levels.
	for (i = 0; i < MAX_PARTY; i++) {
		unsigned int lv = p->party.member[i].lv;
		if (!lv) continue;
		if (p->party.member[i].online
		 && p->party.member[i].char_id != p->family /* In families, the kid is not counted towards exp share rules. */
		) {
			if( lv < p->min_lv ) p->min_lv=lv;
			if( p->max_lv < lv ) p->max_lv=lv;
		}
	}

	if (p->party.exp && !inter_party->check_exp_share(p)) {
		p->party.exp = 0; //Set off even share.
		mapif->party_optionchanged(0, &p->party, 0, 0);
	}
}

// Save party to mysql
static int inter_party_tosql(struct party *p, int flag, int index)
{
	// 'party' ('party_id','name','exp','item','leader_id','leader_char')
	char esc_name[NAME_LENGTH*2+1];// escaped party name
	int party_id;

	if( p == NULL || p->party_id == 0 )
		return 0;
	Assert_ret(index >= 0 && index < MAX_PARTY);
	party_id = p->party_id;

#ifdef NOISY
	ShowInfo("Save party request ("CL_BOLD"%d"CL_RESET" - %s).\n", party_id, p->name);
#endif
	SQL->EscapeStringLen(inter->sql_handle, esc_name, p->name, strnlen(p->name, NAME_LENGTH));

	if( flag & PS_BREAK )
	{// Break the party
		// we'll skip name-checking and just reset everyone with the same party id [celest]
		if( SQL_ERROR == SQL->Query(inter->sql_handle, "UPDATE `%s` SET `party_id`='0' WHERE `party_id`='%d'", char_db, party_id) )
			Sql_ShowDebug(inter->sql_handle);
		if( SQL_ERROR == SQL->Query(inter->sql_handle, "DELETE FROM `%s` WHERE `party_id`='%d'", party_db, party_id) )
			Sql_ShowDebug(inter->sql_handle);
		//Remove from memory
		idb_remove(inter_party->db, party_id);
		return 1;
	}

	if( flag & PS_CREATE )
	{// Create party
		if( SQL_ERROR == SQL->Query(inter->sql_handle, "INSERT INTO `%s` "
			"(`name`, `exp`, `item`, `leader_id`, `leader_char`) "
			"VALUES ('%s', '%d', '%d', '%d', '%d')",
			party_db, esc_name, p->exp, p->item, p->member[index].account_id, p->member[index].char_id) )
		{
			Sql_ShowDebug(inter->sql_handle);
			return 0;
		}
		party_id = p->party_id = (int)SQL->LastInsertId(inter->sql_handle);
	}

	if( flag & PS_BASIC )
	{// Update party info.
		if( SQL_ERROR == SQL->Query(inter->sql_handle, "UPDATE `%s` SET `name`='%s', `exp`='%d', `item`='%d' WHERE `party_id`='%d'",
			party_db, esc_name, p->exp, p->item, party_id) )
			Sql_ShowDebug(inter->sql_handle);
	}

	if( flag & PS_LEADER )
	{// Update leader
		if( SQL_ERROR == SQL->Query(inter->sql_handle, "UPDATE `%s`  SET `leader_id`='%d', `leader_char`='%d' WHERE `party_id`='%d'",
			party_db, p->member[index].account_id, p->member[index].char_id, party_id) )
			Sql_ShowDebug(inter->sql_handle);
	}

	if( flag & PS_ADDMEMBER )
	{// Add one party member.
		if( SQL_ERROR == SQL->Query(inter->sql_handle, "UPDATE `%s` SET `party_id`='%d' WHERE `account_id`='%d' AND `char_id`='%d'",
			char_db, party_id, p->member[index].account_id, p->member[index].char_id) )
			Sql_ShowDebug(inter->sql_handle);
	}

	if( flag & PS_DELMEMBER )
	{// Remove one party member.
		if( SQL_ERROR == SQL->Query(inter->sql_handle, "UPDATE `%s` SET `party_id`='0' WHERE `party_id`='%d' AND `account_id`='%d' AND `char_id`='%d'",
			char_db, party_id, p->member[index].account_id, p->member[index].char_id) )
			Sql_ShowDebug(inter->sql_handle);
	}

	if (chr->show_save_log)
		ShowInfo("Party Saved (%d - %s)\n", party_id, p->name);
	return 1;
}

// Read party from mysql
static struct party_data *inter_party_fromsql(int party_id)
{
	int leader_id = 0;
	int leader_char = 0;
	struct party_data* p;
	struct party_member* m;
	char* data;
	size_t len;
	int i;

#ifdef NOISY
	ShowInfo("Load party request ("CL_BOLD"%d"CL_RESET")\n", party_id);
#endif
	if( party_id <= 0 )
		return NULL;

	//Load from memory
	p = (struct party_data*)idb_get(inter_party->db, party_id);
	if( p != NULL )
		return p;

	p = inter_party->pt;
	memset(p, 0, sizeof(struct party_data));

	if( SQL_ERROR == SQL->Query(inter->sql_handle, "SELECT `party_id`, `name`,`exp`,`item`, `leader_id`, `leader_char` FROM `%s` WHERE `party_id`='%d'", party_db, party_id) )
	{
		Sql_ShowDebug(inter->sql_handle);
		return NULL;
	}

	if( SQL_SUCCESS != SQL->NextRow(inter->sql_handle) )
		return NULL;

	p->party.party_id = party_id;
	SQL->GetData(inter->sql_handle, 1, &data, &len); memcpy(p->party.name, data, min(len, NAME_LENGTH));
	SQL->GetData(inter->sql_handle, 2, &data, NULL); p->party.exp = (atoi(data) ? 1 : 0);
	SQL->GetData(inter->sql_handle, 3, &data, NULL); p->party.item = atoi(data);
	SQL->GetData(inter->sql_handle, 4, &data, NULL); leader_id = atoi(data);
	SQL->GetData(inter->sql_handle, 5, &data, NULL); leader_char = atoi(data);
	SQL->FreeResult(inter->sql_handle);

	// Load members
	if( SQL_ERROR == SQL->Query(inter->sql_handle, "SELECT `account_id`,`char_id`,`name`,`base_level`,`last_map`,`online`,`class` FROM `%s` WHERE `party_id`='%d'", char_db, party_id) )
	{
		Sql_ShowDebug(inter->sql_handle);
		return NULL;
	}
	for( i = 0; i < MAX_PARTY && SQL_SUCCESS == SQL->NextRow(inter->sql_handle); ++i )
	{
		m = &p->party.member[i];
		SQL->GetData(inter->sql_handle, 0, &data, NULL); m->account_id = atoi(data);
		SQL->GetData(inter->sql_handle, 1, &data, NULL); m->char_id = atoi(data);
		SQL->GetData(inter->sql_handle, 2, &data, &len); memcpy(m->name, data, min(len, NAME_LENGTH));
		SQL->GetData(inter->sql_handle, 3, &data, NULL); m->lv = atoi(data);
		SQL->GetData(inter->sql_handle, 4, &data, NULL); m->map = mapindex->name2id(data);
		SQL->GetData(inter->sql_handle, 5, &data, NULL); m->online = (atoi(data) ? 1 : 0);
		SQL->GetData(inter->sql_handle, 6, &data, NULL); m->class = atoi(data);
		m->leader = (m->account_id == leader_id && m->char_id == leader_char ? 1 : 0);
	}
	SQL->FreeResult(inter->sql_handle);

	if (chr->show_save_log)
		ShowInfo("Party loaded (%d - %s).\n", party_id, p->party.name);
	//Add party to memory.
	CREATE(p, struct party_data, 1);
	memcpy(p, inter_party->pt, sizeof(struct party_data));
	//init state
	inter_party->calc_state(p);
	idb_put(inter_party->db, party_id, p);
	return p;
}

static int inter_party_sql_init(void)
{
	//memory alloc
	inter_party->db = idb_alloc(DB_OPT_RELEASE_DATA);
	inter_party->pt = (struct party_data*)aCalloc(sizeof(struct party_data), 1);
	if (!inter_party->pt) {
		ShowFatalError("inter_party->sql_init: Out of Memory!\n");
		exit(EXIT_FAILURE);
	}

#if 0 // Enable if you want to do a party_db cleanup (remove parties with no members) on startup.[Skotlex]
	ShowStatus("cleaning party table...\n");
	if( SQL_ERROR == SQL->Query(inter->sql_handle, "DELETE FROM `%s` USING `%s` LEFT JOIN `%s` ON `%s`.leader_id =`%s`.account_id AND `%s`.leader_char = `%s`.char_id WHERE `%s`.account_id IS NULL",
		party_db, party_db, char_db, party_db, char_db, party_db, char_db, char_db) )
		Sql_ShowDebug(inter->sql_handle);
#endif // 0
	return 0;
}

static void inter_party_sql_final(void)
{
	inter_party->db->destroy(inter_party->db, NULL);
	aFree(inter_party->pt);
	return;
}

// Search for the party according to its name
static struct party_data *inter_party_search_partyname(const char *const str)
{
	char esc_name[NAME_LENGTH*2+1];
	char* data;
	struct party_data* p = NULL;

	SQL->EscapeStringLen(inter->sql_handle, esc_name, str, safestrnlen(str, NAME_LENGTH));
	if( SQL_ERROR == SQL->Query(inter->sql_handle, "SELECT `party_id` FROM `%s` WHERE `name`='%s'", party_db, esc_name) )
		Sql_ShowDebug(inter->sql_handle);
	else if( SQL_SUCCESS == SQL->NextRow(inter->sql_handle) ) {
		SQL->GetData(inter->sql_handle, 0, &data, NULL);
		p = inter_party->fromsql(atoi(data));
	}
	SQL->FreeResult(inter->sql_handle);

	return p;
}

// Returns whether this party can keep having exp share or not.
static int inter_party_check_exp_share(struct party_data *const p)
{
	nullpo_ret(p);
	return (p->party.count < 2 || p->max_lv - p->min_lv <= party_share_level);
}

// Is there any member in the party?
static int inter_party_check_empty(struct party_data *p)
{
	int i;
	if (p==NULL||p->party.party_id==0) return 1;
	for(i=0;i<MAX_PARTY && !p->party.member[i].account_id;i++);
	if (i < MAX_PARTY) return 0;
	// If there is no member, then break the party
	mapif->party_broken(p->party.party_id,0);
	inter_party->tosql(&p->party, PS_BREAK, 0);
	return 1;
}

// Create Party
static struct party_data *inter_party_create(const char *name, int item, int item2, const struct party_member *leader)
{
	struct party_data *p;
	int i;
	nullpo_ret(name);
	nullpo_ret(leader);

	if (!*name || inter_party->search_partyname(name) != NULL) {
		return NULL;
	}
	// Check Authorized letters/symbols in the name of the character
	if (char_name_option == 1) { // only letters/symbols in char_name_letters are authorized
		for (i = 0; i < NAME_LENGTH && name[i]; i++)
			if (strchr(char_name_letters, name[i]) == NULL) {
				if (name[i] == '"') { /* client-special-char */
					char *newname = aStrndup(name, NAME_LENGTH-1);
					normalize_name(newname,"\"");
					trim(newname);
					p = inter_party->create(newname, item, item2, leader);
					aFree(newname);
					return p;
				}
				return NULL;
			}
	} else if (char_name_option == 2) { // letters/symbols in char_name_letters are forbidden
		for (i = 0; i < NAME_LENGTH && name[i]; i++)
			if (strchr(char_name_letters, name[i]) != NULL) {
				return NULL;
			}
	}

	p = (struct party_data*)aCalloc(1, sizeof(struct party_data));

	safestrncpy(p->party.name, name, NAME_LENGTH);
	p->party.exp=0;
	p->party.item=(item?1:0)|(item2?2:0);

	memcpy(&p->party.member[0], leader, sizeof(struct party_member));
	p->party.member[0].leader=1;
	p->party.member[0].online=1;

	p->party.party_id=-1;//New party.
	if (!inter_party->tosql(&p->party, PS_CREATE | PS_ADDMEMBER, 0)) {
		aFree(p);
		return NULL;
	}

	//Add party to db
	inter_party->calc_state(p);
	idb_put(inter_party->db, p->party.party_id, p);

	return p;
}

// Add a player to party request
static bool inter_party_add_member(int party_id, const struct party_member *member)
{
	struct party_data *p;
	int i;

	nullpo_ret(member);
	p = inter_party->fromsql(party_id);
	if( p == NULL || p->size == MAX_PARTY ) {
		return false;
	}

	ARR_FIND( 0, MAX_PARTY, i, p->party.member[i].account_id == 0 );
	if (i == MAX_PARTY) {
		// Party full
		return false;
	}

	memcpy(&p->party.member[i], member, sizeof(struct party_member));
	p->party.member[i].leader = 0;
	if (p->party.member[i].online) p->party.count++;
	p->size++;
	if (p->size == 2 || p->size == 3) // Check family state. And also accept either of their Parents. [RoM]
		inter_party->calc_state(p);
	else //Check even share range.
	if (member->lv < p->min_lv || member->lv > p->max_lv || p->family) {
		if (p->family) p->family = 0; //Family state broken.
		inter_party->check_lv(p);
	}

	mapif->party_info(-1, &p->party, 0);
	inter_party->tosql(&p->party, PS_ADDMEMBER, i);

	return true;
}

//Party setting change request
static bool inter_party_change_option(int party_id, int account_id, int exp, int item, int map_fd)
{
	struct party_data *p;
	int flag = 0;
	p = inter_party->fromsql(party_id);

	if(!p)
		return false;

	p->party.exp=exp;
	if( exp && !inter_party->check_exp_share(p) ){
		flag|=0x01;
		p->party.exp=0;
	}
	p->party.item = item&0x3; //Filter out invalid values.
	mapif->party_optionchanged(map_fd, &p->party, account_id, flag);
	inter_party->tosql(&p->party, PS_BASIC, 0);
	return true;
}

//Request leave party
static bool inter_party_leave(int party_id, int account_id, int char_id)
{
	struct party_data *p;
	int i,j;

	p = inter_party->fromsql(party_id);
	if( p == NULL )
	{// Party does not exists?
		if( SQL_ERROR == SQL->Query(inter->sql_handle, "UPDATE `%s` SET `party_id`='0' WHERE `party_id`='%d'", char_db, party_id) )
			Sql_ShowDebug(inter->sql_handle);
		return false;
	}

	for (i = 0; i < MAX_PARTY; i++) {
		if(p->party.member[i].account_id == account_id &&
			p->party.member[i].char_id == char_id) {
			break;
		}
	}
	if (i >= MAX_PARTY)
		return false; //Member not found?

	mapif->party_withdraw(party_id, account_id, char_id);

	j = p->party.member[i].lv;
	if (p->party.member[i].online > 0) {
		p->party.member[i].online = 0;
		p->party.count--;
	}
	inter_party->tosql(&p->party, PS_DELMEMBER, i);
	memset(&p->party.member[i], 0, sizeof(struct party_member));
	p->size--;
	if (j == p->min_lv || j == p->max_lv || p->family) {
		if(p->family) p->family = 0; //Family state broken.
		inter_party->check_lv(p);
	}

	if (inter_party->check_empty(p) == 0) {
		mapif->party_info(-1, &p->party, 0);
	}
	return true;
}

// When member goes to other map or levels up.
static bool inter_party_change_map(int party_id, int account_id, int char_id, unsigned short map, int online, unsigned int lv)
{
	struct party_data *p;
	int i;

	p = inter_party->fromsql(party_id);
	if (p == NULL)
		return false;

	for(i = 0; i < MAX_PARTY &&
		(p->party.member[i].account_id != account_id ||
		p->party.member[i].char_id != char_id); i++);

	if (i == MAX_PARTY)
		return false;

	if (p->party.member[i].online != online)
	{
		p->party.member[i].online = online;
		if (online)
			p->party.count++;
		else
			p->party.count--;
		// Even share check situations: Family state (always breaks)
		// character logging on/off is max/min level (update level range)
		// or character logging on/off has a different level (update level range using new level)
		if (p->family ||
			(p->party.member[i].lv <= p->min_lv || p->party.member[i].lv >= p->max_lv) ||
			(p->party.member[i].lv != lv && (lv <= p->min_lv || lv >= p->max_lv))
			)
		{
			p->party.member[i].lv = lv;
			inter_party->check_lv(p);
		}
		//Send online/offline update.
		mapif->party_membermoved(&p->party, i);
	}

	if (p->party.member[i].lv != lv) {
		if(p->party.member[i].lv == p->min_lv ||
			p->party.member[i].lv == p->max_lv)
		{
			p->party.member[i].lv = lv;
			inter_party->check_lv(p);
		} else
			p->party.member[i].lv = lv;
		//There is no need to send level update to map servers
		//since they do nothing with it.
	}

	if (p->party.member[i].map != map) {
		p->party.member[i].map = map;
		mapif->party_membermoved(&p->party, i);
	}
	return true;
}

//Request party dissolution
static bool inter_party_disband(int party_id)
{
	struct party_data *p;

	p = inter_party->fromsql(party_id);

	if(!p)
		return false;
	inter_party->tosql(&p->party,PS_BREAK,0);
	mapif->party_broken(party_id, 1);
	return 0;
}

static bool inter_party_change_leader(int party_id, int account_id, int char_id)
{
	struct party_data *p;
	int i;

	p = inter_party->fromsql(party_id);

	if(!p)
		return false;

	for (i = 0; i < MAX_PARTY; i++) {
		if(p->party.member[i].leader)
			p->party.member[i].leader = 0;
		if(p->party.member[i].account_id == account_id && p->party.member[i].char_id == char_id) {
			p->party.member[i].leader = 1;
			inter_party->tosql(&p->party,PS_LEADER, i);
		}
	}
	return true;
}

// Communication from the map server
//-Analysis that only one packet
// Data packet length is set to inter.c that you
// Do NOT go and check the packet length, RFIFOSKIP is done by the caller
// Return :
//  0 : error
//  1 : ok
static int inter_party_parse_frommap(int fd)
{
	RFIFOHEAD(fd);
	switch(RFIFOW(fd,0)) {
	case 0x3020: mapif->parse_CreateParty(fd, RFIFOP(fd,4), RFIFOB(fd,28), RFIFOB(fd,29), RFIFOP(fd,30)); break;
	case 0x3021: mapif->parse_PartyInfo(fd, RFIFOL(fd,2), RFIFOL(fd,6)); break;
	case 0x3022: mapif->parse_PartyAddMember(fd, RFIFOL(fd,4), RFIFOP(fd,8)); break;
	case 0x3023: mapif->parse_PartyChangeOption(fd, RFIFOL(fd,2), RFIFOL(fd,6), RFIFOW(fd,10), RFIFOW(fd,12)); break;
	case 0x3024: mapif->parse_PartyLeave(fd, RFIFOL(fd,2), RFIFOL(fd,6), RFIFOL(fd,10)); break;
	case 0x3025: mapif->parse_PartyChangeMap(fd, RFIFOL(fd,2), RFIFOL(fd,6), RFIFOL(fd,10), RFIFOW(fd,14), RFIFOB(fd,16), RFIFOW(fd,17)); break;
	case 0x3026: mapif->parse_BreakParty(fd, RFIFOL(fd,2)); break;
	case 0x3029: mapif->parse_PartyLeaderChange(fd, RFIFOL(fd,2), RFIFOL(fd,6), RFIFOL(fd,10)); break;
	default:
		return 0;
	}
	return 1;
}

static int inter_party_CharOnline(int char_id, int party_id)
{
	struct party_data* p;
	int i;

	if( party_id == -1 )
	{// Get party_id from the database
		char* data;

		if( SQL_ERROR == SQL->Query(inter->sql_handle, "SELECT party_id FROM `%s` WHERE char_id='%d'", char_db, char_id) )
		{
			Sql_ShowDebug(inter->sql_handle);
			return 0;
		}

		if( SQL_SUCCESS != SQL->NextRow(inter->sql_handle) )
			return 0; //Eh? No party?

		SQL->GetData(inter->sql_handle, 0, &data, NULL);
		party_id = atoi(data);
		SQL->FreeResult(inter->sql_handle);
	}
	if (party_id == 0)
		return 0; //No party...

	p = inter_party->fromsql(party_id);
	if(!p) {
		ShowError("Character %d's party %d not found!\n", char_id, party_id);
		return 0;
	}

	//Set member online
	for(i=0; i<MAX_PARTY; i++) {
		if (p->party.member[i].char_id == char_id) {
			if (!p->party.member[i].online) {
				p->party.member[i].online = 1;
				p->party.count++;
				if (p->party.member[i].lv < p->min_lv ||
					p->party.member[i].lv > p->max_lv)
					inter_party->check_lv(p);
			}
			break;
		}
	}
	return 1;
}

static int inter_party_CharOffline(int char_id, int party_id)
{
	struct party_data *p=NULL;
	int i;

	if( party_id == -1 )
	{// Get guild_id from the database
		char* data;

		if( SQL_ERROR == SQL->Query(inter->sql_handle, "SELECT party_id FROM `%s` WHERE char_id='%d'", char_db, char_id) )
		{
			Sql_ShowDebug(inter->sql_handle);
			return 0;
		}

		if( SQL_SUCCESS != SQL->NextRow(inter->sql_handle) )
			return 0; //Eh? No party?

		SQL->GetData(inter->sql_handle, 0, &data, NULL);
		party_id = atoi(data);
		SQL->FreeResult(inter->sql_handle);
	}
	if (party_id == 0)
		return 0; //No party...

	//Character has a party, set character offline and check if they were the only member online
	if ((p = inter_party->fromsql(party_id)) == NULL)
		return 0;

	//Set member offline
	for(i=0; i< MAX_PARTY; i++) {
		if(p->party.member[i].char_id == char_id)
		{
			p->party.member[i].online = 0;
			p->party.count--;
			if(p->party.member[i].lv == p->min_lv ||
				p->party.member[i].lv == p->max_lv)
				inter_party->check_lv(p);
			break;
		}
	}

	if(!p->party.count)
		//Parties don't have any data that needs be saved at this point... so just remove it from memory.
		idb_remove(inter_party->db, party_id);
	return 1;
}

void inter_party_defaults(void)
{
	inter_party = &inter_party_s;

	inter_party->pt = NULL;
	inter_party->db = NULL;

	inter_party->sql_init = inter_party_sql_init;
	inter_party->sql_final = inter_party_sql_final;
	inter_party->check_lv = inter_party_check_lv;
	inter_party->calc_state = inter_party_calc_state;
	inter_party->tosql = inter_party_tosql;
	inter_party->fromsql = inter_party_fromsql;
	inter_party->search_partyname = inter_party_search_partyname;
	inter_party->check_exp_share = inter_party_check_exp_share;
	inter_party->check_empty = inter_party_check_empty;
	inter_party->parse_frommap = inter_party_parse_frommap;
	inter_party->leave = inter_party_leave;
	inter_party->CharOnline = inter_party_CharOnline;
	inter_party->CharOffline = inter_party_CharOffline;

	inter_party->create = inter_party_create;
	inter_party->add_member = inter_party_add_member;
	inter_party->change_option = inter_party_change_option;
	inter_party->change_map = inter_party_change_map;
	inter_party->disband = inter_party_disband;
	inter_party->change_leader = inter_party_change_leader;
}
