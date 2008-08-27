// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/db.h"
#include "../common/mmo.h"
#include "../common/showmsg.h"
#include "../common/socket.h"
#include "../common/strlib.h"
#include "../common/timer.h"
#include "chardb.h"
#include "charlog.h"
#include "int_guild.h"
#include "int_storage.h"
#include "inter.h"
#include "map.h"
#include <stdio.h>

//temporary imports
#include "char.h"
extern struct mmo_charstatus *char_dat;
extern int char_num, char_max;
extern int login_fd;
extern uint32 login_ip;
extern char login_ip_str[128];
#define AUTH_TIMEOUT 30000
struct auth_node {
	int account_id;
	int char_id;
	uint32 login_id1;
	uint32 login_id2;
	uint32 ip;
	int sex;
	time_t expiration_time; // # of seconds 1/1/1970 (timestamp): Validity limit of the account (0 = unlimited)
	int gmlevel;
};
extern DBMap* auth_db;
struct online_char_data {
	int account_id;
	int char_id;
	int fd;
	int waiting_disconnect;
	short server; // -2: unknown server, -1: not connected, 0+: id of server
};
extern DBMap* online_char_db;
extern uint32 char_ip;
extern char char_ip_str[128];
extern int send_accounts_tologin(int tid, unsigned int tick, int id, intptr data);
extern void char_auth_ok(int fd, struct char_session_data *sd);
extern int disconnect_player(int account_id);
extern int char_delete(struct mmo_charstatus *cs);
extern int mapif_disconnectplayer(int fd, int account_id, int char_id, int reason);
extern int chardb_waiting_disconnect(int tid, unsigned int tick, int id, intptr data);
extern void set_char_offline(int char_id, int account_id);
#include "../common/sql.h"



int parse_fromlogin(int fd)
{
	int i;
	struct char_session_data *sd;

	// only login-server can have an access to here.
	// so, if it isn't the login-server, we disconnect the session.
	if( fd != login_fd )
		set_eof(fd);

	if(session[fd]->flag.eof) {
		if (fd == login_fd) {
			ShowWarning("Connection to login-server lost (connection #%d).\n", fd);
			login_fd = -1;
		}
		do_close(fd);
		return 0;
	}

	sd = (struct char_session_data*)session[fd]->session_data;

	while(RFIFOREST(fd) >= 2)
	{
		uint16 command = RFIFOW(fd,0);

		switch( command )
		{

		// acknowledgement of connect-to-loginserver request
		case 0x2711:
			if (RFIFOREST(fd) < 3)
				return 0;

			if( RFIFOB(fd,2) ) {
				ShowError("Can not connect to login-server.\n");
				ShowError("The server communication passwords (default s1/p1) are probably invalid.\n");
#ifdef TXT_ONLY
				ShowInfo("Also, please make sure your accounts file (default: accounts.txt) has those values present.\n");
#else
				ShowInfo("Also, please make sure your login db has the correct communication username/passwords and the gender of the account is S.\n");
#endif
				ShowInfo("The communication passwords can be changed in map_athena.conf and char_athena.conf\n");
			} else {
				ShowStatus("Connected to login-server (connection #%d).\n", fd);
				
				//Send online accounts to login server.
				send_accounts_tologin(-1, gettick(), 0, 0);

				// if no map-server already connected, display a message...
				ARR_FIND( 0, MAX_MAP_SERVERS, i, server[i].fd > 0 && server[i].map[0] );
				if( i == MAX_MAP_SERVERS )
					ShowStatus("Awaiting maps from map-server.\n");
			}
			RFIFOSKIP(fd,3);
		break;

		// acknowledgement of account authentication request
		case 0x2713:
			if (RFIFOREST(fd) < 60)
				return 0;
		{
			int account_id = RFIFOL(fd,2);
			int login_id1 = RFIFOL(fd,6);
			int login_id2 = RFIFOL(fd,10);
			bool result = RFIFOB(fd,14);
			const char* email = (const char*)RFIFOP(fd,15);
			time_t expiration_time = (time_t)RFIFOL(fd,55);
			int gmlevel = RFIFOB(fd,59);

			// find the session with this account id
			ARR_FIND( 0, fd_max, i, session[i] && (sd = (struct char_session_data*)session[i]->session_data) &&
				sd->account_id == account_id && sd->login_id1 == login_id1 && sd->login_id2 == login_id2 );
			if( i < fd_max )
			{
				if( result ) { // failure
					WFIFOHEAD(i,3);
					WFIFOW(i,0) = 0x6c;
					WFIFOB(i,2) = 0x42;
					WFIFOSET(i,3);
				} else { // success
					memcpy(sd->email, email, 40);
					sd->expiration_time = expiration_time;
					sd->gmlevel = gmlevel;
					char_auth_ok(i, sd);
				}
			}
		}
			RFIFOSKIP(fd,60);
		break;

		// acknowledgement of e-mail/limited time request
		case 0x2717:
			if (RFIFOREST(fd) < 51)
				return 0;

			// find the session with this account id
			ARR_FIND( 0, fd_max, i, session[i] && (sd = (struct char_session_data*)session[i]->session_data) && sd->account_id == RFIFOL(fd,2) );
			if( i < fd_max )
			{
				memcpy(sd->email, RFIFOP(fd,6), 40);
				sd->expiration_time = (time_t)RFIFOL(fd,46);
				sd->gmlevel = RFIFOB(fd,50);
			}
			RFIFOSKIP(fd,51);
		break;

		// login-server alive packet
		case 0x2718:
			if (RFIFOREST(fd) < 2)
				return 0;
			RFIFOSKIP(fd,2);
		break;

		// changesex reply
		case 0x2723:
			if (RFIFOREST(fd) < 7)
				return 0;
		{
			unsigned char buf[7];

			int acc = RFIFOL(fd,2);
			int sex = RFIFOB(fd,6);
			RFIFOSKIP(fd,7);

			if( acc > 0 )
			{
#ifdef TXT_ONLY
				int i, j;

				struct auth_node* node = (struct auth_node*)idb_get(auth_db, acc);
				if( node != NULL )
					node->sex = sex;

				for( i = 0; i < char_num; ++i )
				if( char_dat[i].account_id == acc )
				{
					int jobclass = char_dat[i].class_;
					char_dat[i].sex = sex;
					if (jobclass == JOB_BARD || jobclass == JOB_DANCER ||
					    jobclass == JOB_CLOWN || jobclass == JOB_GYPSY ||
					    jobclass == JOB_BABY_BARD || jobclass == JOB_BABY_DANCER) {
						// job modification
						if (jobclass == JOB_BARD || jobclass == JOB_DANCER) {
							char_dat[i].class_ = (sex) ? JOB_BARD : JOB_DANCER;
						} else if (jobclass == JOB_CLOWN || jobclass == JOB_GYPSY) {
							char_dat[i].class_ = (sex) ? JOB_CLOWN : JOB_GYPSY;
						} else if (jobclass == JOB_BABY_BARD || jobclass == JOB_BABY_DANCER) {
							char_dat[i].class_ = (sex) ? JOB_BABY_BARD : JOB_BABY_DANCER;
						}
						// remove specifical skills of classes 19, 4020 and 4042
						for(j = 315; j <= 322; j++) {
							if (char_dat[i].skill[j].id > 0 && !char_dat[i].skill[j].flag) {
								if (char_dat[i].skill_point > USHRT_MAX - char_dat[i].skill[j].lv)
									char_dat[i].skill_point = USHRT_MAX;
								else
									char_dat[i].skill_point += char_dat[i].skill[j].lv;
								char_dat[i].skill[j].id = 0;
								char_dat[i].skill[j].lv = 0;
							}
						}
						// remove specifical skills of classes 20, 4021 and 4043
						for(j = 323; j <= 330; j++) {
							if (char_dat[i].skill[j].id > 0 && !char_dat[i].skill[j].flag) {
								if (char_dat[i].skill_point > USHRT_MAX - char_dat[i].skill[j].lv)
									char_dat[i].skill_point = USHRT_MAX;
								else
									char_dat[i].skill_point += char_dat[i].skill[j].lv;

								char_dat[i].skill[j].id = 0;
								char_dat[i].skill[j].lv = 0;
							}
						}
					}
					// to avoid any problem with equipment and invalid sex, equipment is unequiped.
					for (j = 0; j < MAX_INVENTORY; j++) {
						if (char_dat[i].inventory[j].nameid && char_dat[i].inventory[j].equip)
							char_dat[i].inventory[j].equip = 0;
					}
					char_dat[i].weapon = 0;
					char_dat[i].shield = 0;
					char_dat[i].head_top = 0;
					char_dat[i].head_mid = 0;
					char_dat[i].head_bottom = 0;

					if (char_dat[i].guild_id)	//If there is a guild, update the guild_member data [Skotlex]
						inter_guild_sex_changed(char_dat[i].guild_id, acc, char_dat[i].char_id, sex);
				}
#else
				int char_id[MAX_CHARS];
				int class_[MAX_CHARS];
				int guild_id[MAX_CHARS];
				int num;
				int i;
				char* data;

				struct auth_node* node = (struct auth_node*)idb_get(auth_db, acc);
				if( node != NULL )
					node->sex = sex;

				// get characters
				if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`,`class`,`guild_id` FROM `%s` WHERE `account_id` = '%d'", char_db, acc) )
					Sql_ShowDebug(sql_handle);
				for( i = 0; i < MAX_CHARS && SQL_SUCCESS == Sql_NextRow(sql_handle); ++i )
				{
					Sql_GetData(sql_handle, 0, &data, NULL); char_id[i] = atoi(data);
					Sql_GetData(sql_handle, 1, &data, NULL); class_[i] = atoi(data);
					Sql_GetData(sql_handle, 2, &data, NULL); guild_id[i] = atoi(data);
				}
				num = i;
				for( i = 0; i < num; ++i )
				{
					if( class_[i] == JOB_BARD || class_[i] == JOB_DANCER ||
						class_[i] == JOB_CLOWN || class_[i] == JOB_GYPSY ||
						class_[i] == JOB_BABY_BARD || class_[i] == JOB_BABY_DANCER )
					{
						// job modification
						if( class_[i] == JOB_BARD || class_[i] == JOB_DANCER )
							class_[i] = (sex ? JOB_BARD : JOB_DANCER);
						else if( class_[i] == JOB_CLOWN || class_[i] == JOB_GYPSY )
							class_[i] = (sex ? JOB_CLOWN : JOB_GYPSY);
						else if( class_[i] == JOB_BABY_BARD || class_[i] == JOB_BABY_DANCER )
							class_[i] = (sex ? JOB_BABY_BARD : JOB_BABY_DANCER);
						// remove specifical skills of classes 19,20 4020,4021 and 4042,4043
						if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `skill_point` = `skill_point` +"
							" (SELECT SUM(lv) FROM `%s` WHERE `char_id` = '%d' AND `id` >= '315' AND `id` <= '330' AND `lv` > '0')"
							" WHERE `char_id` = '%d'",
							char_db, skill_db, char_id[i], char_id[i]) )
							Sql_ShowDebug(sql_handle);
						if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `char_id` = '%d' AND `id` >= '315' AND `id` <= '330'", skill_db, char_id[i]) )
							Sql_ShowDebug(sql_handle);
					}
					// to avoid any problem with equipment and invalid sex, equipment is unequiped.
					if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `equip` = '0' WHERE `char_id` = '%d'", inventory_db, char_id[i]) )
						Sql_ShowDebug(sql_handle);
					if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `class`='%d', `weapon`='0', `shield`='0', `head_top`='0', `head_mid`='0', `head_bottom`='0' WHERE `char_id`='%d'", char_db, class_[i], char_id[i]) )
						Sql_ShowDebug(sql_handle);

					if( guild_id[i] )// If there is a guild, update the guild_member data [Skotlex]
						inter_guild_sex_changed(guild_id[i], acc, char_id[i], sex);
				}
				Sql_FreeResult(sql_handle);
#endif

				// disconnect player if online on char-server
				disconnect_player(acc);
			}

			// notify all mapservers about this change
			WBUFW(buf,0) = 0x2b0d;
			WBUFL(buf,2) = acc;
			WBUFB(buf,6) = sex;
			mapif_sendall(buf, 7);
		}
		break;

		case 0x2726:	// Request to send a broadcast message (no answer)
			if (RFIFOREST(fd) < 8 || RFIFOREST(fd) < (8 + RFIFOL(fd,4)))
				return 0;
			if (RFIFOL(fd,4) < 1)
				char_log("Receiving a message for broadcast, but message is void.\n");
			else
			{
				// at least 1 map-server
				ARR_FIND( 0, MAX_MAP_SERVERS, i, server[i].fd >= 0 );
				if (i == MAX_MAP_SERVERS)
					char_log("'ladmin': Receiving a message for broadcast, but no map-server is online.\n");
				else {
					unsigned char buf[128];
					char message[4096]; // +1 to add a null terminated if not exist in the packet
					int lp;
					char *p;
					memset(message, '\0', sizeof(message));
					memcpy(message, RFIFOP(fd,8), RFIFOL(fd,4));
					message[sizeof(message)-1] = '\0';
					remove_control_chars(message);
					// remove all first spaces
					p = message;
					while(p[0] == ' ')
						p++;
					// if message is only composed of spaces
					if (p[0] == '\0')
						char_log("Receiving a message for broadcast, but message is only a lot of spaces.\n");
					// else send message to all map-servers
					else {
						if (RFIFOW(fd,2) == 0) {
							char_log("'ladmin': Receiving a message for broadcast (message (in yellow): %s)\n",
							         message);
							lp = 4;
						} else {
							char_log("'ladmin': Receiving a message for broadcast (message (in blue): %s)\n",
							         message);
							lp = 8;
						}
						// split message to max 80 char
						while(p[0] != '\0') { // if not finish
							if (p[0] == ' ') // jump if first char is a space
								p++;
							else {
								char split[80];
								char* last_space;
								sscanf(p, "%79[^\t]", split); // max 79 char, any char (\t is control char and control char was removed before)
								split[sizeof(split)-1] = '\0'; // last char always \0
								if ((last_space = strrchr(split, ' ')) != NULL) { // searching space from end of the string
									last_space[0] = '\0'; // replace it by NULL to have correct length of split
									p++; // to jump the new NULL
								}
								p += strlen(split);
								// send broadcast to all map-servers
								WBUFW(buf,0) = 0x3800;
								WBUFW(buf,2) = lp + strlen(split) + 1;
								WBUFL(buf,4) = 0x65756c62; // only write if in blue (lp = 8)
								memcpy(WBUFP(buf,lp), split, strlen(split) + 1);
								mapif_sendall(buf, WBUFW(buf,2));
							}
						}
					}
				}
			}
			RFIFOSKIP(fd,8 + RFIFOL(fd,4));
		break;

		// reply to an account_reg2 registry request
		case 0x2729:
			if (RFIFOREST(fd) < 4 || RFIFOREST(fd) < RFIFOW(fd,2))
				return 0;

		{	//Receive account_reg2 registry, forward to map servers.
			unsigned char buf[13+ACCOUNT_REG2_NUM*sizeof(struct global_reg)];
			memcpy(buf,RFIFOP(fd,0), RFIFOW(fd,2));
			WBUFW(buf,0) = 0x3804; //Map server can now receive all kinds of reg values with the same packet. [Skotlex]
			mapif_sendall(buf, WBUFW(buf,2));
		}
			RFIFOSKIP(fd, RFIFOW(fd,2));
		break;

#ifdef TXT_ONLY
		// Account deletion notification (from login-server)
		case 0x2730:
			if (RFIFOREST(fd) < 6)
				return 0;
			// Deletion of all characters of the account
			for(i = 0; i < char_num; i++) {
				if (char_dat[i].account_id == RFIFOL(fd,2)) {
					char_delete(&char_dat[i]);
					if (i < char_num - 1) {
						memcpy(&char_dat[i], &char_dat[char_num-1], sizeof(struct mmo_charstatus));
						// if moved character owns to deleted account, check again it's character
						if (char_dat[i].account_id == RFIFOL(fd,2)) {
							i--;
						// Correct moved character reference in the character's owner by [Yor]
						} else {
							int j, k;
							struct char_session_data *sd2;
							for (j = 0; j < fd_max; j++) {
								if (session[j] && (sd2 = (struct char_session_data*)session[j]->session_data) &&
									sd2->account_id == char_dat[char_num-1].account_id) {
									for (k = 0; k < MAX_CHARS; k++) {
										if (sd2->found_char[k] == char_num-1) {
											sd2->found_char[k] = i;
											break;
										}
									}
									break;
								}
							}
						}
					}
					char_num--;
				}
			}
			// Deletion of the storage
			inter_storage_delete(RFIFOL(fd,2));
			// send to all map-servers to disconnect the player
			{
				unsigned char buf[6];
				WBUFW(buf,0) = 0x2b13;
				WBUFL(buf,2) = RFIFOL(fd,2);
				mapif_sendall(buf, 6);
			}
			// disconnect player if online on char-server
			disconnect_player(RFIFOL(fd,2));
			RFIFOSKIP(fd,6);
		break;
#endif

		// State change of account/ban notification (from login-server)
		case 0x2731:
			if (RFIFOREST(fd) < 11)
				return 0;
			
		{	// send to all map-servers to disconnect the player
			unsigned char buf[11];
			WBUFW(buf,0) = 0x2b14;
			WBUFL(buf,2) = RFIFOL(fd,2);
			WBUFB(buf,6) = RFIFOB(fd,6); // 0: change of statut, 1: ban
			WBUFL(buf,7) = RFIFOL(fd,7); // status or final date of a banishment
			mapif_sendall(buf, 11);
		}
			// disconnect player if online on char-server
			disconnect_player(RFIFOL(fd,2));

			RFIFOSKIP(fd,11);
		break;

		// Login server request to kick a character out. [Skotlex]
		case 0x2734:
			if (RFIFOREST(fd) < 6)
				return 0;
		{
			int aid = RFIFOL(fd,2);
			struct online_char_data* character = (struct online_char_data*)idb_get(online_char_db, aid);
			RFIFOSKIP(fd,6);
			if( character != NULL )
			{// account is already marked as online!
				if( character->server > -1 )
				{	//Kick it from the map server it is on.
					mapif_disconnectplayer(server[character->server].fd, character->account_id, character->char_id, 2);
					if (character->waiting_disconnect == -1)
						character->waiting_disconnect = add_timer(gettick()+AUTH_TIMEOUT, chardb_waiting_disconnect, character->account_id, 0);
				}
				else
				{// Manual kick from char server.
					struct char_session_data *tsd;
					int i;
					ARR_FIND( 0, fd_max, i, session[i] && (tsd = (struct char_session_data*)session[i]->session_data) && tsd->account_id == aid );
					if( i < fd_max )
					{
						WFIFOHEAD(i,3);
						WFIFOW(i,0) = 0x81;
						WFIFOB(i,2) = 2; // "Someone has already logged in with this id"
						WFIFOSET(i,3);
						set_eof(i);
					}
					else //Shouldn't happen, but just in case.
						set_char_offline(-1, aid);
				}
			}
		}
		break;
		
		// ip address update signal from login server
		case 0x2735:
		{
			unsigned char buf[2];
			uint32 new_ip = 0;

			WBUFW(buf,0) = 0x2b1e;
			mapif_sendall(buf, 2);

			new_ip = host2ip(login_ip_str);
			if (new_ip && new_ip != login_ip)
				login_ip = new_ip; //Update login up.

			new_ip = host2ip(char_ip_str);
			if (new_ip && new_ip != char_ip)
			{	//Update ip.
				char_ip = new_ip;
				ShowInfo("Updating IP for [%s].\n", char_ip_str);
				// notify login server about the change
				WFIFOHEAD(fd,6);
				WFIFOW(fd,0) = 0x2736;
				WFIFOL(fd,2) = htonl(char_ip);
				WFIFOSET(fd,6);
			}
		}

			RFIFOSKIP(fd,2);
		break;

		default:
			ShowError("Unknown packet 0x%04x received from login-server, disconnecting.\n", command);
			set_eof(fd);
			return 0;
		}
	}

	RFIFOFLUSH(fd);
	return 0;
}
