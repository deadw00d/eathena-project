// $Id: mob.c,v 1.7 2004/09/25 05:32:18 MouseJstr Exp $
#include "base.h"
#include "timer.h"
#include "socket.h"
#include "db.h"
#include "nullpo.h"
#include "malloc.h"
#include "utils.h"
#include "showmsg.h"
#include "../common/dbaccess.h"

#include "map.h"
#include "clif.h"
#include "intif.h"
#include "pc.h"
#include "status.h"
#include "mob.h"
#include "guild.h"
#include "itemdb.h"
#include "skill.h"
#include "battle.h"
#include "party.h"
#include "npc.h"
#include "log.h"
#include "script.h"



#define MIN_MOBTHINKTIME 100

#define MOB_LAZYMOVEPERC 50	// Move probability in the negligent mode MOB (rate of 1000 minute)
#define MOB_LAZYWARPPERC 20	// Warp probability in the negligent mode MOB (rate of 1000 minute)

struct mob_db mob_db[MAX_MOB_DB+1];

#define CLASSCHANGE_BOSS_NUM 21

/*==========================================
 * Local prototype declaration   (only required thing)
 *------------------------------------------
 */
int mob_makedummymobdb(int);
int mob_timer(int tid,unsigned long tick,int id,int data);
int mobskill_deltimer(struct mob_data &md);
int mob_skillid2skillidx(int class_,unsigned short skillid);
int mobskill_use_id(struct mob_data &md,struct block_list *target,unsigned short skill_idx);
int mob_walktoxy_sub(struct mob_data &md);

/*==========================================
 * Mob is searched with a name.
 *------------------------------------------
 */
int mobdb_searchname(const char *str)
{
	size_t i;

	for(i=0;i<sizeof(mob_db)/sizeof(mob_db[0]);i++){
		if( strcasecmp(mob_db[i].name,str)==0 || strcmp(mob_db[i].jname,str)==0 ||
			memcmp(mob_db[i].name,str,24)==0 || memcmp(mob_db[i].jname,str,24)==0)
			return i;
	}

	return 0;
}

/*==========================================
 * Id Mob is checked.
 *------------------------------------------
 */
int mobdb_checkid(const unsigned long id)
{
	if (id <= 0 || id >= (sizeof(mob_db) / sizeof(mob_db[0])) || mob_db[id].name[0] == '\0')
		return 0;

	return id;
}

/*==========================================
 * The minimum data set for MOB spawning
 *------------------------------------------
 */
int mob_spawn_dataset(struct mob_data &md,const char *mobname,int class_)
{
	md.bl.prev=NULL;
	md.bl.next=NULL;
	if(strcmp(mobname,"--en--")==0)
		safestrcpy(md.name,mob_db[class_].name,24);
	else if(strcmp(mobname,"--ja--")==0)
		safestrcpy(md.name,mob_db[class_].jname,24);
	else
		safestrcpy(md.name,mobname,24);

	md.base_class = md.class_ = class_;
	md.bl.id = npc_get_new_npc_id();

	memset(&md.state,0,sizeof(md.state));
	md.timer = -1;
	md.target_id=0;
	md.attacked_id=0;
	md.attacked_count=0;
	md.speed=mob_db[class_].speed;
	md.mode =mob_db[class_].mode;

	return 0;
}


/*==========================================
 * The MOB appearance for one time (for scripts)
 *------------------------------------------
 */
int mob_once_spawn (struct map_session_data *sd, const char *mapname,
	int x, int y, const char *mobname, int class_, int amount, const char *event)
{
	struct mob_data *md = NULL;
	int m, count, lv = 255;
	int i, j;
	bool random=false;

	if(sd) lv = sd->status.base_level;

	if(sd && strcmp(mapname,"this")==0)
		m = sd->bl.m;
	else
		m = map_mapname2mapid(mapname);

	if (m < 0 || m>(int)map_num || amount <= 0 || (class_ >= 0 && class_ <= 1000) || class_ > MAX_MOB_DB + 4000)	// �l���ُ�Ȃ珢�����~�߂�
		return 0;

	if (class_ < 0)
	{	// �����_���ɏ���
		int k;
		random = true;
		i = 0;
		j = -class_-1;
		if(j >= 0 && j < MAX_RANDOMMONSTER)
		{
			do
			{
				class_ = rand() % 1000 + 1001;
				k = rand() % 1000000;
			} while((mob_db[class_].max_hp <= 0 || mob_db[class_].summonper[j] <= k ||
					(battle_config.random_monster_checklv && lv < mob_db[class_].lv)) && (i++) < 2000);
			if(i >= 2000)
				class_ = mob_db[0].summonper[j];
		}
		else
			return 0;
//		if(battle_config.etc_log)
//			ShowMessage("mobclass=%d try=%d\n",class_,i);
	}
	if(sd)
	{	//even if the coords were wrong, spawn mob anyways (but look for most suitable coords first) Got from Freya [Lupus]
		if (x <= 0 || y <= 0) {
			if (x <= 0) x = sd->bl.x + rand() % 3 - 1;
			if (y <= 0) y = sd->bl.y + rand() % 3 - 1;
			if (map_getcell(m, x, y, CELL_CHKNOPASS)) {
				x = sd->bl.x;
				y = sd->bl.y;
			}
		}
	}
	else if (x <= 0 || y <= 0)
	{
		i = j = 0;
		ShowMessage("mob_once_spawn: ?? %i %i %p (%s,%s)\n", x,y,sd,mapname,event);
		do {
			x = rand() % (map[m].xs - 2) + 1;
			y = rand() % (map[m].ys - 2) + 1;
		} while ((i = map_getcell(m, x, y, CELL_CHKNOPASS)) && j++ < 64);
		if (i) { // not solved?
			x = 0;
			y = 0;
		}
	}

	for (count = 0; count < amount; count++)
	{
		md = (struct mob_data *)aCalloc(1,sizeof(struct mob_data));

		if (class_ > MAX_MOB_DB + 2000)
		{	// large/tiny mobs [Valaris]
			md->state.size = 2;
			class_ -= (MAX_MOB_DB + 2000);
		}
		else if (class_ > MAX_MOB_DB)
		{
			md->state.size = 1;
			class_ -= MAX_MOB_DB;
		}

		if(mob_db[class_].mode & 0x02)
			md->lootitem = (struct item *)aCalloc(LOOTITEM_SIZE,sizeof(struct item));
		else
			md->lootitem = NULL;

		mob_spawn_dataset(*md, mobname, class_);
		md->bl.m = m;
		md->bl.x = x;
		md->bl.y = y;
		md->bl.type = BL_MOB;
		map_addiddb (md->bl);

		//�ړ����ăA�N�e�B�u�Ŕ�������
		if(random && battle_config.dead_branch_active)
			md->mode |= 0x1 | 0x4 | 0x80;

		safestrcpy(md->npc_event, event, sizeof(md->npc_event));

		mob_spawn(md->bl.id);

		if(class_ == 1288)
		{	// emperium hp based on defense level [Valaris]
			struct guild_castle *gc = guild_mapname2gc(map[md->bl.m].mapname);
			if(gc)
			{
				md->max_hp += 2000 * gc->defense;
				md->hp = md->max_hp;
			}
		}	// end addition [Valaris]
	}
	return (amount > 0) ? md->bl.id : 0;
}
/*==========================================
 * The MOB appearance for one time (& area specification for scripts)
 *------------------------------------------
 */
int mob_once_spawn_area(struct map_session_data *sd,const char *mapname,
	int x0,int y0,int x1,int y1,
	const char *mobname,int class_,int amount,const char *event)
{
	int x,y,i,max,dx=-1,dy=-1,id=0;
	int m;

	if(sd && strcmp(mapname,"this")==0)
		m=sd->bl.m;
	else
		m=map_mapname2mapid(mapname);

	// A summon is stopped if a value is unusual
	if(m<0 || m>(int)map_num || amount<=0 || (class_>=0 && class_<=1000) || class_>MAX_MOB_DB)
		return 0;

	max=(y1-y0+1)*(x1-x0+1)*3;
	if(max>1000) max=1000;

	for(i=0;i<amount;i++){
		int j=0;
		do{
			x=rand()%(x1-x0+1)+x0;
			y=rand()%(y1-y0+1)+y0;
		} while (map_getcell(m,x,y,CELL_CHKNOPASS) && (++j)<max);

		if(j>=max){
			if(dx>=0){	// Since reference went wrong, the place which boiled before is used.
				x=dx;
				y=dy;
			}else
				return 0;	// Since reference of the place which boils first went wrong, it stops.
		}
		if(x==0||y==0)
			ShowMessage("xory=0, x=%d,y=%d,x0=%d,y0=%d\n",x,y,x0,y0);
		id=mob_once_spawn(sd,mapname,x,y,mobname,class_,1,event);
		dx=x;
		dy=y;
	}
	return id;
}

/*==========================================
 * Summoning Guardians [Valaris]
 *------------------------------------------
 */
int mob_spawn_guardian(struct map_session_data *sd,const char *mapname,
	int x,int y,const char *mobname,int class_,int amount,const char *event,int guardian)
{
	struct mob_data *md=NULL;
	int m,count=1,lv=255;

	if( sd )
		lv=sd->status.base_level;

	if( sd && strcmp(mapname,"this")==0)
		m=sd->bl.m;
	else
		m=map_mapname2mapid(mapname);

	if(m<0 || amount<=0 || (class_>=0 && class_<=1000) || class_>MAX_MOB_DB)	// Invalid monster classes
		return 0;

	if(class_<0)
		return 0;

	if(sd){
		if(x<=0) x=sd->bl.x;
		if(y<=0) y=sd->bl.y;
	}

	else if(x<=0 || y<=0)
		ShowMessage("mob_spawn_guardian: ??\n");


	for(count=0;count<amount;count++){
		struct guild_castle *gc;
		md = (struct mob_data*)aCalloc(1,sizeof(struct mob_data));

		mob_spawn_dataset(*md,mobname,class_);
		md->bl.m=m;
		md->bl.x=x;
		md->bl.y=y;
		md->bl.type=BL_MOB;
		map_addiddb(md->bl);

		safestrcpy(md->npc_event,event, sizeof(md->npc_event));
		mob_spawn(md->bl.id);

		gc=guild_mapname2gc(map[md->bl.m].mapname);
		if(gc)
		{
			md->max_hp += 2000 * gc->defense;
			if(guardian==0) { md->hp=gc->Ghp0; gc->GID0=md->bl.id; }
			if(guardian==1) { md->hp=gc->Ghp1; gc->GID1=md->bl.id; }
			if(guardian==2) { md->hp=gc->Ghp2; gc->GID2=md->bl.id; }
			if(guardian==3) { md->hp=gc->Ghp3; gc->GID3=md->bl.id; }
			if(guardian==4) { md->hp=gc->Ghp4; gc->GID4=md->bl.id; }
			if(guardian==5) { md->hp=gc->Ghp5; gc->GID5=md->bl.id; }
			if(guardian==6) { md->hp=gc->Ghp6; gc->GID6=md->bl.id; }
			if(guardian==7) { md->hp=gc->Ghp7; gc->GID7=md->bl.id; }
		}
	}

	return (amount>0)?md->bl.id:0;
}

/*==========================================
 * The disregard ID is added to mob.
 *------------------------------------------
 */
int mob_exclusion_add(struct mob_data &md,int type,unsigned long id)
{
	if(type==1)
		md.exclusion_src=id;
	if(type==2)
		md.exclusion_party=id;
	if(type==3)
		md.exclusion_guild=id;

	return 0;
}

/*==========================================
 * The disregard ID of mob is checked. (TAGE?)
 *------------------------------------------
 */
int mob_exclusion_check(struct mob_data &md,struct map_session_data *sd)
{
	nullpo_retr(0, sd);

	if(sd->bl.type==BL_PC){
		if(md.exclusion_src && md.exclusion_src==sd->bl.id)
			return 1;
		if(md.exclusion_party && md.exclusion_party==sd->status.party_id)
			return 2;
		if(md.exclusion_guild && md.exclusion_guild==sd->status.guild_id)
			return 3;
	}
	return 0;
}

/*==========================================
 * Appearance income of mob
 *------------------------------------------
 */
int mob_get_viewclass(int class_)
{
	return mob_db[class_].view_class;
}
int mob_get_sex(int class_)
{
	return mob_db[class_].sex;
}
short mob_get_hair(int class_)
{
	return mob_db[class_].hair;
}
short mob_get_hair_color(int class_)
{
	return mob_db[class_].hair_color;
}
short mob_get_weapon(int class_)
{
	return mob_db[class_].weapon;
}
short mob_get_shield(int class_)
{
	return mob_db[class_].shield;
}
short mob_get_head_top(int class_)
{
	return mob_db[class_].head_top;
}
short mob_get_head_mid(int class_)
{
	return mob_db[class_].head_mid;
}
short mob_get_head_buttom(int class_)
{
	return mob_db[class_].head_buttom;
}
short mob_get_clothes_color(int class_) // Add for player monster dye - Valaris
{
	return mob_db[class_].clothes_color;  // End
}
int mob_get_equip(int class_) // mob equip [Valaris]
{
	return mob_db[class_].equip;
}
/*==========================================
 * Is MOB in the state in which the present movement is possible or not?
 *------------------------------------------
 */
int mob_can_move(struct mob_data &md)
{
	if( DIFF_TICK(md.canmove_tick,gettick())>0  || (md.opt1 > 0 && md.opt1 != 6) || md.option&2)
		return 0;
	// �A���N�����œ����Ȃ��Ƃ�
	if( md.sc_data[SC_ANKLE].timer != -1 || //�A���N���X�l�A
		md.sc_data[SC_AUTOCOUNTER].timer != -1 || //�I�[�g�J�E���^�[
		md.sc_data[SC_BLADESTOP].timer != -1 || //���n���
		md.sc_data[SC_SPIDERWEB].timer != -1  //�X�p�C�_�[�E�F�b�u
		)
		return 0;

	return 1;
}

/*==========================================
 * Time calculation concerning one step next to mob
 *------------------------------------------
 */
int calc_next_walk_step(struct mob_data &md)
{
	if(md.walkpath.path_pos>=md.walkpath.path_len)
		return -1;
	if(md.walkpath.path[md.walkpath.path_pos]&1)
		return status_get_speed(&md.bl)*14/10;
	return status_get_speed(&md.bl);
}


/*==========================================
 * Mob Walk processing
 *------------------------------------------
 */
int mob_walk(struct mob_data &md,unsigned long tick,int data)
{
	int moveblock;
	int i;
	static int dirx[8]={0,-1,-1,-1,0,1,1,1};
	static int diry[8]={1,1,0,-1,-1,-1,0,1};
	int x,y,dx,dy;

	md.state.state=MS_IDLE;
	if(md.walkpath.path_pos>=md.walkpath.path_len || md.walkpath.path_pos!=data)
		return 0;

	md.walkpath.path_half ^= 1;
	if(md.walkpath.path_half==0){
		md.walkpath.path_pos++;
		if(md.state.change_walk_target){
			mob_walktoxy_sub(md);
			return 0;
		}
	}
	else {
		if(md.walkpath.path[md.walkpath.path_pos]>=8)
			return 1;

		x = md.bl.x;
		y = md.bl.y;
		if(map_getcell(md.bl.m,x,y,CELL_CHKNOPASS)) {
			mob_stop_walking(md,1);
			return 0;
		}
		md.dir=md.walkpath.path[md.walkpath.path_pos];
		dx = dirx[md.dir];
		dy = diry[md.dir];

		if (map_getcell(md.bl.m,x+dx,y+dy,CELL_CHKBASILICA) && !(status_get_mode(&md.bl)&0x20)) {
			mob_stop_walking(md,1);
			return 0;
		}

		if (map_getcell(md.bl.m,x+dx,y+dy,CELL_CHKNOPASS)) {
			mob_walktoxy_sub(md);
			return 0;
		}

		if (skill_check_moonlit (&md.bl,x+dx,y+dy)) {
			mob_walktoxy_sub(md);
			return 0;
		}
		moveblock = ( x/BLOCK_SIZE != (x+dx)/BLOCK_SIZE || y/BLOCK_SIZE != (y+dy)/BLOCK_SIZE);

		md.state.state=MS_WALK;
		map_foreachinmovearea(clif_moboutsight,md.bl.m,x-AREA_SIZE,y-AREA_SIZE,x+AREA_SIZE,y+AREA_SIZE,dx,dy,BL_PC,&md);

		x += dx;
		y += dy;
		if(md.min_chase>13)
			md.min_chase--;

		skill_unit_move(md.bl,tick,0);
		if(moveblock) map_delblock(md.bl);
		md.bl.x = x;
		md.bl.y = y;
		if(moveblock) map_addblock(md.bl);
		skill_unit_move(md.bl,tick,1);

		map_foreachinmovearea(clif_mobinsight,md.bl.m,x-AREA_SIZE,y-AREA_SIZE,x+AREA_SIZE,y+AREA_SIZE,-dx,-dy,BL_PC,&md);
		md.state.state=MS_IDLE;

		if(md.option&4)
			skill_check_cloaking(&md.bl);
	}
	if((i=calc_next_walk_step(md))>0){
		i = i>>1;
		if(i < 1 && md.walkpath.path_half == 0)
			i = 1;


		if(md.timer != -1)
		{
			delete_timer(md.timer,mob_timer);
			md.timer=-1;
		}
		md.timer=add_timer(tick+i,mob_timer,md.bl.id,md.walkpath.path_pos);
		md.state.state=MS_WALK;

		if(md.walkpath.path_pos>=md.walkpath.path_len)
			clif_fixmobpos(md);	// �Ƃ܂����Ƃ��Ɉʒu�̍đ��M
	}
	return 0;
}

/*==========================================
 * Attack processing of mob
 *------------------------------------------
 */
int mob_attack(struct mob_data &md,unsigned long tick,int data)
{
	struct block_list *tbl=NULL;
	struct map_session_data *tsd=NULL;
	struct mob_data *tmd=NULL;

	int mode,race,range;

	md.min_chase=13;
	md.state.state=MS_IDLE;
	md.state.skillstate=MSS_IDLE;

	if( md.skilltimer!=-1 )	// �X�L���g�p��
		return 0;

	if(md.opt1>0 || md.option&2)
		return 0;

	if(md.sc_data[SC_AUTOCOUNTER].timer != -1)
		return 0;

	if(md.sc_data[SC_BLADESTOP].timer != -1)
		return 0;

	tbl = map_id2bl(md.target_id);
	if(tbl == NULL){
		md.target_id=0;
		md.state.targettype = NONE_ATTACKABLE;
		return 0;
	}

	if(tbl->type==BL_PC)
		tsd=(struct map_session_data *)tbl;
	else if(tbl->type==BL_MOB)
		tmd=(struct mob_data *)tbl;
	else
		return 0;

	if(tsd){
		if( pc_isdead(*tsd) || tsd->invincible_timer != -1 ||  pc_isinvisible(*tsd) || md.bl.m != tbl->m || tbl->prev == NULL || distance(md.bl.x,md.bl.y,tbl->x,tbl->y)>=13 ){
			mob_stopattack(md); //Stop attacking once target has been defeated/unreachable.[Skotlex]
			return 0;
		}
	}
	if(tmd){
		if(md.bl.m != tbl->m || tbl->prev == NULL || distance(md.bl.x,md.bl.y,tbl->x,tbl->y)>=13){
			mob_stopattack(md); //Stop attacking once target has been defeated/unreachable.[Skotlex]
			return 0;
		}
	}


	if(!md.mode)
		mode=mob_db[md.class_].mode;
	else
		mode=md.mode;

	race=mob_db[md.class_].race;
	if(!(mode&0x80)){
		md.target_id=0;
		md.state.targettype = NONE_ATTACKABLE;
		return 0;
	}
	if(tsd && !(mode&0x20) && (tsd->sc_data[SC_TRICKDEAD].timer != -1 || tsd->sc_data[SC_BASILICA].timer != -1 ||
		 ((pc_ishiding(*tsd) || tsd->state.gangsterparadise) && !((race == 4 || race == 6 || mode&0x100) && !tsd->state.perfect_hiding) ) ) ) {
		md.target_id=0;
		md.state.targettype = NONE_ATTACKABLE;
		return 0;
	}

	range = mob_db[md.class_].range;
	if(mode&1)
		range++;
	if(distance(md.bl.x,md.bl.y,tbl->x,tbl->y) > range)
		return 0;
	if(battle_config.monster_attack_direction_change)
		md.dir=map_calc_dir(md.bl, tbl->x,tbl->y );	// �����ݒ�

	//clif_fixmobpos(md);

	md.state.skillstate=MSS_ATTACK;
	if( mobskill_use(md,tick,-2) )	// �X�L���g�p
		return 0;

	md.target_lv = battle_weapon_attack(&md.bl,tbl,tick,0);

	if(!(battle_config.monster_cloak_check_type&2) && md.sc_data[SC_CLOAKING].timer != -1)
		status_change_end(&md.bl,SC_CLOAKING,-1);

	md.attackabletime = tick + status_get_adelay(&md.bl);
	if(md.timer != -1)
	{
		delete_timer(md.timer,mob_timer);
		md.timer=-1;
	}
	md.timer=add_timer(md.attackabletime,mob_timer,md.bl.id,0);
	md.state.state=MS_ATTACK;

	return 0;
}


/*==========================================
 * The attack of PC which is attacking id is stopped.
 * The callback function of clif_foreachclient
 *------------------------------------------
 */
int mob_stopattacked(struct map_session_data &sd,va_list ap)
{
	unsigned long id;

	nullpo_retr(0, ap);
	id=va_arg(ap,unsigned long);
	if(sd.attacktarget==id)
		pc_stopattack(sd);
	return 0;
}
/*==========================================
 * The timer in which the mob's states changes
 *------------------------------------------
 */
int mob_changestate(struct mob_data &md,int state,int type)
{
	unsigned long tick;
	int i;

	if(md.timer != -1)
	{
		delete_timer(md.timer,mob_timer);
		md.timer=-1;
	}
	md.state.state=state;

	switch(state){
	case MS_WALK:
		if((i=calc_next_walk_step(md))>0){
			i = i>>2;
			md.timer=add_timer(gettick()+i,mob_timer,md.bl.id,0);
		}
		else
			md.state.state=MS_IDLE;
		break;
	case MS_ATTACK:
		tick = gettick();
		i=DIFF_TICK(md.attackabletime,tick);
		if(i>0 && i<2000)
			md.timer=add_timer(md.attackabletime,mob_timer,md.bl.id,0);
		else if(type) {
			md.attackabletime = tick + status_get_amotion(&md.bl);
			md.timer=add_timer(md.attackabletime,mob_timer,md.bl.id,0);
		}
		else {
			md.attackabletime = tick + 1;
			md.timer=add_timer(md.attackabletime,mob_timer,md.bl.id,0);
		}
		break;
	case MS_DELAY:
		md.timer=add_timer(gettick()+type,mob_timer,md.bl.id,0);
		break;
	case MS_DEAD:
		skill_castcancel(&md.bl,0);
		mobskill_deltimer(md);
		md.state.skillstate=MSS_DEAD;
		md.last_deadtime=gettick();
		// Since it died, all aggressors' attack to this mob is stopped.
		clif_foreachclient(mob_stopattacked,md.bl.id);
		skill_unit_move(md.bl,gettick(),0);
		status_change_clear(&md.bl,2);	// �X�e�[�^�X�ُ����������
		skill_clear_unitgroup(&md.bl);	// �S�ẴX�L�����j�b�g�O���[�v���폜����
		skill_cleartimerskill(&md.bl);
		if(md.deletetimer!=-1)
			delete_timer(md.deletetimer,mob_timer_delete);
		md.deletetimer=-1;
		md.hp = md.target_id = md.attacked_id = md.attacked_count = 0;
		md.state.targettype = NONE_ATTACKABLE;
		break;
	}

	return 0;
}

/*==========================================
 * timer processing of mob (timer function)
 * It branches to a walk and an attack.
 *------------------------------------------
 */
int mob_timer(int tid,unsigned long tick,int id,int data)
{
	struct mob_data *md;
	struct block_list *bl;

	if( (bl=map_id2bl(id)) == NULL ){ //�U�����Ă����G���������Ȃ��̂͐���̂悤��
		return 1;
	}

	if(!bl || !bl->type || bl->type!=BL_MOB)
		return 1;

	nullpo_retr(1, md=(struct mob_data*)bl);

	if(md->timer != tid){
		if(battle_config.error_log)
			ShowMessage("mob_timer %d != %d\n",md->timer,tid);
		return 0;
	}
	md->timer=-1;
	if(md->bl.prev == NULL || md->state.state == MS_DEAD)
		return 1;

	map_freeblock_lock();
	switch(md->state.state){
	case MS_WALK:
		mob_walk(*md,tick,data);
		break;
	case MS_ATTACK:
		mob_attack(*md,tick,data);
		break;
	case MS_DELAY:
		mob_changestate(*md,MS_IDLE,0);
		break;
	default:
		if(battle_config.error_log)
			ShowMessage("mob_timer : %d ?\n",md->state.state);
		break;
	}

	if (md->timer == -1)
		mob_changestate(*md,MS_WALK,0);

	map_freeblock_unlock();
	return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int mob_walktoxy_sub(struct mob_data &md)
{
	struct walkpath_data wpd;
	int x,y;
	static int dirx[8]={0,-1,-1,-1,0,1,1,1};
	static int diry[8]={1,1,0,-1,-1,-1,0,1};

	memset(&wpd, 0, sizeof(wpd));

	if(path_search(wpd,md.bl.m,md.bl.x,md.bl.y,md.to_x,md.to_y,md.state.walk_easy))
		return 1;
	if (wpd.path[0] >= 8)
		return 1;
	x = md.bl.x+dirx[wpd.path[0]];
	y = md.bl.y+diry[wpd.path[0]];
	md.state.change_walk_target=0;

	if (map_getcell(md.bl.m,x,y,CELL_CHKBASILICA) && !(status_get_mode(&md.bl)&0x20))
	{
		return 1;
	}
	memcpy(&md.walkpath,&wpd,sizeof(wpd));
	mob_changestate(md,MS_WALK,0);
	clif_movemob(md);

	return 0;
}

/*==========================================
 * mob move start
 *------------------------------------------
 */
int mob_walktoxy(struct mob_data &md,int x,int y,int easy)
{
	struct walkpath_data wpd;

	if(md.state.state == MS_WALK && path_search(wpd,md.bl.m,md.bl.x,md.bl.y,x,y,easy) )
		return 1;

	md.state.walk_easy = easy;
	md.to_x=x;
	md.to_y=y;
	if(md.state.state == MS_WALK)
		md.state.change_walk_target=1;
	else
		return mob_walktoxy_sub(md);

	return 0;
}

/*==========================================
 * mob spawn with delay (timer function)
 *------------------------------------------
 */
int mob_delayspawn(int tid,unsigned long tick,int m,int n)
{
	mob_spawn(m);
	return 0;
}

/*==========================================
 * spawn timing calculation
 *------------------------------------------
 */
int mob_setdelayspawn(unsigned long id)
{
	unsigned long spawntime,spawntime1,spawntime2,spawntime3;
	unsigned short mode, delayrate = 100; //for battle config delays
	struct block_list *bl = map_id2bl(id);
	struct mob_data *md = (struct mob_data *)bl;

	if( bl == NULL || bl->type != BL_MOB )
		return -1;

	// Processing of MOB which is not revitalized
	if( !md->cache )
	{
		mob_unload(*md);
		return 0;
	}

	//Apply the spawn delay fix [Skotlex]
	mode = status_get_mode(bl);
	if (mode & 0x20) {	//Bosses
		if (battle_config.boss_spawn_delay != 100)
			delayrate = delayrate*battle_config.boss_spawn_delay/100;
	} else if (mode&0x40) {	//Plants
		if (battle_config.plant_spawn_delay != 100)
			delayrate = delayrate*battle_config.plant_spawn_delay/100;
	} else if (battle_config.mob_spawn_delay != 100) //Normal mobs
		delayrate = delayrate*battle_config.mob_spawn_delay/100;

	spawntime1=md->last_spawntime+(md->cache->delay1*delayrate/100);
	spawntime2=md->last_deadtime +(md->cache->delay2*delayrate/100);
	spawntime3 = gettick() + 5000 + rand()%5000; //Lupus
	// spawntime = max(spawntime1,spawntime2,spawntime3);
	if (DIFF_TICK(spawntime1, spawntime2) > 0)
		spawntime = spawntime1;
	else
		spawntime = spawntime2;
	if (DIFF_TICK(spawntime3, spawntime) > 0)
		spawntime = spawntime3;

	add_timer(spawntime, mob_delayspawn, id, 0);
	return 0;
}

/*==========================================
 * Mob spawning. Initialization is also variously here.
 *------------------------------------------
 */
int mob_spawn(unsigned long id)
{
	int x=0,y=0,i=0;
	unsigned long c, tick = gettick();
	struct mob_data *md;
	struct block_list *bl;

	if ((bl = map_id2bl(id)) == NULL || bl->type != BL_MOB)
		return -1;
	nullpo_retr(-1, md = (struct mob_data*)bl);

	md->last_spawntime = tick;

	if (md->bl.prev != NULL)
		map_delblock(md->bl);

	if( md->class_ != md->base_class )
	{	// respawning a morphed mob
		md->class_ = md->base_class;

		if(md->cache)
			memcpy(md->name, md->cache->mobname,24);
		else // no chance to find the real name, just take it from the db, should not happen anyway
			memcpy(md->name, mob_db[md->base_class].jname,24);
		md->speed=mob_db[md->base_class].speed;
		md->mode=mob_db[md->base_class].mode;
	}

	md->target_dir = 0;

	memset(&md->state, 0, sizeof(md->state));
	md->attacked_id = 0;
	md->attacked_count = 0;
	md->target_id = 0;
	md->move_fail_count = 0;

	if (!md->speed)
		md->speed = mob_db[md->class_].speed;
	md->def_ele = mob_db[md->class_].element;

	if (!md->level) // [Valaris]
		md->level=mob_db[md->class_].lv;

	md->master_id = 0;
	md->master_dist = 0;

	md->state.state = MS_IDLE;
	md->state.skillstate = MSS_IDLE;
	md->timer = -1;
	md->last_thinktime = tick;
	md->next_walktime = tick+rand()%4000+1000;
	md->attackabletime = tick;
	md->canmove_tick = tick;

	md->guild_id = 0;
	if (md->class_ >= 1285 && md->class_ <= 1288) {
		struct guild_castle *gc=guild_mapname2gc(map[md->bl.m].mapname);
		if(gc)
			md->guild_id = gc->guild_id;
	}

	md->deletetimer = -1;
	md->skilltimer = -1;

	for (i = 0, c = tick-1000*3600*10; i < MAX_MOBSKILL; i++)
		md->skilldelay[i] = c;
	md->skillid = 0;
	md->skilllv = 0;

	memset(md->dmglog, 0, sizeof(md->dmglog));
	if (md->lootitem)
		memset(md->lootitem, 0, sizeof(md->lootitem));
	md->lootitem_count = 0;

	for (i = 0; i < MAX_MOBSKILLTIMERSKILL; i++)
		md->skilltimerskill[i].timer = -1;

	for (i = 0; i < MAX_STATUSCHANGE; i++) {
		md->sc_data[i].timer = -1;
		md->sc_data[i].val1 = md->sc_data[i].val2 = md->sc_data[i].val3 = md->sc_data[i].val4 = 0;
	}
	md->opt1 = md->opt2 = md->opt3 = md->option = 0;

	memset(md->skillunit, 0, sizeof(md->skillunit));
	memset(md->skillunittick, 0, sizeof(md->skillunittick));

	md->max_hp = mob_db[md->class_].max_hp;
	md->hp = status_get_max_hp(&md->bl);
	if (md->hp <= 0) {
		mob_makedummymobdb(md->class_);
		md->hp = status_get_max_hp(&md->bl);
	}

	i=0;
	do
	{
		if(!md->cache)
		{
			x=md->bl.x;
			y=md->bl.y;
		}
		else if( md->cache->x0==0 && md->cache->y0==0 )
		{
			x = rand()%(map[md->bl.m].xs-2)+1;
			y = rand()%(map[md->bl.m].ys-2)+1;
		}
		else
		{
			x = md->cache->x0+rand()%(md->cache->xs+1)-md->cache->xs/2;
			y = md->cache->y0+rand()%(md->cache->ys+1)-md->cache->ys/2;
		}
		i++;
	} while(map_getcell(md->bl.m,x,y,CELL_CHKNOPASS) && i < 50);

	if (i >= 50) {
		// retry again later
		add_timer(tick+5000,mob_delayspawn,id,0);
		return 1;
	}

	md->to_x = md->bl.x = x;
	md->to_y = md->bl.y = y;
	md->dir=rand()%8;

	map_addblock(md->bl);
	skill_unit_move(md->bl,tick,1);
	clif_spawnmob(*md);

	return 0;
}


/*==========================================
 * The stop of MOB's attack
 *------------------------------------------
 */
int mob_stopattack(struct mob_data &md)
{
	md.target_id = 0;
	md.state.targettype = NONE_ATTACKABLE;
	md.attacked_id = 0;
	md.attacked_count = 0;
	return 0;
}
/*==========================================
 * The stop of MOB's walking
 *------------------------------------------
 */
int mob_stop_walking(struct mob_data &md,int type)
{
	if(md.state.state == MS_WALK || md.state.state == MS_IDLE)
	{
		int dx=0,dy=0;

		md.walkpath.path_len=0;
		if(type&4){
			dx=md.to_x-md.bl.x;
			if(dx<0)
				dx=-1;
			else if(dx>0)
				dx=1;
			dy=md.to_y-md.bl.y;
			if(dy<0)
				dy=-1;
			else if(dy>0)
				dy=1;
		}
		md.to_x=md.bl.x+dx;
		md.to_y=md.bl.y+dy;
		if(dx!=0 || dy!=0){
			mob_walktoxy_sub(md);
			return 0;
		}
		mob_changestate(md,MS_IDLE,0);
	}
	if(type&0x01)
		clif_fixmobpos(md);
	if(type&0x02) {
		int delay=status_get_dmotion(&md.bl);
		unsigned long tick = gettick();
		if(battle_config.monster_damage_delay && DIFF_TICK(md.canmove_tick,tick)<0 )
			md.canmove_tick = tick + delay;
	}

	return 0;
}

/*==========================================
 * Reachability to a Specification ID existence place
 *------------------------------------------
 */
int mob_can_reach(struct mob_data &md,struct block_list &bl,int range)
{
	int dx,dy;
	struct walkpath_data wpd;
	int i;

	if( md.bl.m != bl.m)	// �Ⴄ��b�v
		return 0;

	dx=abs(bl.x - md.bl.x);
	dy=abs(bl.y - md.bl.y);

	if( range>0 && range < ((dx>dy)?dx:dy) )	// ��������
		return 0;

	if( md.bl.x==bl.x && md.bl.y==bl.y )	// �����}�X
		return 1;

	//=========== guildcastle guardian no search start===========
	//when players are the guild castle member not attack them !
	/*if(md->class_ >= 1285 && md->class_ <= 1287){
		struct map_session_data *sd;
		struct guild *g=NULL;
		struct guild_castle *gc=guild_mapname2gc(map[bl->m].name);

		if(gc && agit_flag==0)	// Guardians will not attack during non-woe time [Valaris]
			return 0;  // end addition [Valaris]

		if(gc && bl->type == BL_PC){
			nullpo_retr(0, sd=(struct map_session_data *)bl);
			if(gc && sd->status.guild_id > 0) {
				g=guild_search(sd->status.guild_id);	// don't attack guild members [Valaris]
				if(g && g->guild_id == gc->guild_id)
						return 0;
				if(g && gc && guild_isallied(*g,*gc))
						return 0;
			}
		}
	}*/
	//========== guildcastle guardian no search eof==============

	/*if(bl->type == BL_PC && battle_config.monsters_ignore_gm) {	 // option to have monsters ignore GMs [Valaris]
		struct map_session_data *sd;
		if((sd=(struct map_session_data *)bl) != NULL && pc_isGM(sd) >= battle_config.monsters_ignore_gm)
			return 0;
  	}*/

	// Obstacle judging
	wpd.path_len=0;
	wpd.path_pos=0;
	wpd.path_half=0;
	if(path_search(wpd,md.bl.m,md.bl.x,md.bl.y,bl.x,bl.y,0)!=-1)
		return 1;

	if(bl.type!=BL_PC && bl.type!=BL_MOB)
		return 0;

	// It judges whether it can adjoin or not.
	dx=(dx>0)?1:((dx<0)?-1:0);
	dy=(dy>0)?1:((dy<0)?-1:0);
	if(path_search(wpd,md.bl.m,md.bl.x,md.bl.y,bl.x-dx,bl.y-dy,0)!=-1)
		return 1;
	for(i=0;i<9;i++){
		if(path_search(wpd,md.bl.m,md.bl.x,md.bl.y,bl.x-1+i/3,bl.y-1+i%3,0)!=-1)
			return 1;
	}
	return 0;
}

/*==========================================
 * Determination for an attack of a monster
 *------------------------------------------
 */
int mob_target(struct mob_data &md,struct block_list *bl,int dist)
{
	struct map_session_data *sd;
	struct status_change *sc_data;
	short *option;
	int mode,race;

	nullpo_retr(0, bl);

	sc_data = status_get_sc_data(bl);
	option = status_get_option(bl);
	race=mob_db[md.class_].race;

	if(!md.mode)
		mode=mob_db[md.class_].mode;
	else
		mode=md.mode;
	if(!(mode&0x80)) {
		md.target_id = 0;
		return 0;
	}
	// Nothing will be carried out if there is no mind of changing TAGE by TAGE ending.
	if( (md.target_id > 0 && md.state.targettype == ATTACKABLE) && (!(mode&0x04) || rand()%100>25) &&
		// if the monster was provoked ignore the above rule [celest]
		!(md.provoke_id && md.provoke_id == bl->id))
		return 0;
	if(mode&0x20 ||	// Coercion is exerted if it is MVPMOB.
		(sc_data && sc_data[SC_TRICKDEAD].timer == -1 && sc_data[SC_BASILICA].timer == -1 &&
		 ( (option && !(*option&0x06) ) || race==4 || race==6 || mode&0x100 ) ) )
	{
		if(bl->type == BL_PC)
		{
			sd = (struct map_session_data *)bl;
			if(sd->invincible_timer != -1 || pc_isinvisible(*sd))
				return 0;
			if(!(mode&0x20) && race!=4 && race!=6 && !(mode&0x100) && sd->state.gangsterparadise)
				return 0;
		}
		md.target_id = bl->id;	// Since there was no disturbance, it locks on to target.
		if(bl->type == BL_PC || bl->type == BL_MOB)
			md.state.targettype = ATTACKABLE;
		else
			md.state.targettype = NONE_ATTACKABLE;
		if (md.provoke_id)
			md.provoke_id = 0;
		md.min_chase=dist+13;
		if(md.min_chase>26)
			md.min_chase=26;
	}
	return 0;
}

/*==========================================
 * The ?? routine of an active monster
 *------------------------------------------
 */
int mob_ai_sub_hard_activesearch(struct block_list &bl,va_list ap)
{
	int mode,race,dist,*pcc;
	struct mob_data *smd;
	nullpo_retr(0, ap);
	nullpo_retr(0, smd=va_arg(ap,struct mob_data *));
	nullpo_retr(0, pcc=va_arg(ap,int *));

	if(bl.type!=BL_PC && bl.type!=BL_MOB)
		return 0;


	//�G��������
	if(battle_check_target(&smd->bl,&bl,BCT_ENEMY)==0)
		return 0;

	if(!smd->mode)
		mode=mob_db[smd->class_].mode;
	else
		mode=smd->mode;

	// �A�N�e�B�u�Ń^�[�Q�b�g�˒����ɂ���Ȃ�A���b�N����
	if( mode&0x04 )
	{
		race=mob_db[smd->class_].race;

		if(bl.type==BL_PC)
		{	//�Ώۂ�PC�̏ꍇ
			struct map_session_data &tsd=(struct map_session_data &)bl;

			if( !pc_isdead(tsd) &&
				tsd.bl.m == smd->bl.m &&
				tsd.invincible_timer == -1 &&
				!pc_isinvisible(tsd) &&
				(dist=distance(smd->bl.x,smd->bl.y,tsd.bl.x,tsd.bl.y))<9 )
			if( mode&0x20 ||
				(tsd.sc_data[SC_TRICKDEAD].timer == -1 && tsd.sc_data[SC_BASILICA].timer == -1 &&
				((!pc_ishiding(tsd) && !tsd.state.gangsterparadise) || ((race == 4 || race == 6 || mode&0x100)
				&& !tsd.state.perfect_hiding) )))
			{	// �W�Q���Ȃ�������


				if( ((mob_db[smd->class_].range > 6) || mob_can_reach(*smd,bl,12)) && 	// ���B�\������
					rand()%1000<1000/(++(*pcc)) )	// �͈͓�PC�œ��m���ɂ���
				{
					smd->target_id=tsd.bl.id;
					smd->state.targettype = ATTACKABLE;
					smd->min_chase=13;
				}
			}
		}
		else if(bl.type==BL_MOB)
		{	//�Ώۂ�Mob�̏ꍇ
			struct mob_data &tmd=(struct mob_data &)bl;
			if( tmd.bl.m == smd->bl.m &&
				(dist=distance(smd->bl.x,smd->bl.y,tmd.bl.x,tmd.bl.y))<9 )
			{
				if( mob_can_reach(*smd,bl,12) && 		// ���B�\������
					rand()%1000<1000/(++(*pcc)) )
				{	// �͈͓��œ��m���ɂ���
					smd->target_id=bl.id;
					smd->state.targettype = ATTACKABLE;
					smd->min_chase=13;
				}
			}
		}
	}
	return 0;
}

/*==========================================
 * loot monster item search
 *------------------------------------------
 */
int mob_ai_sub_hard_lootsearch(struct block_list &bl,va_list ap)
{
	struct mob_data* md;
	int mode,dist,*itc;

	nullpo_retr(0, ap);
	nullpo_retr(0, md=va_arg(ap,struct mob_data *));
	nullpo_retr(0, itc=va_arg(ap,int *));

	if(!md->mode)
		mode=mob_db[md->class_].mode;
	else
		mode=md->mode;


	if( !md->target_id && mode&0x02){
		if(!md->lootitem || (battle_config.monster_loot_type == 1 && md->lootitem_count >= LOOTITEM_SIZE) )
			return 0;
		if(bl.m == md->bl.m && (dist=distance(md->bl.x,md->bl.y,bl.x,bl.y))<9){
			if( mob_can_reach(*md,bl,12) && 		// Reachability judging
				rand()%1000<1000/(++(*itc)) ){	// It is made a probability, such as within the limits PC.
				md->target_id=bl.id;
				md->state.targettype = NONE_ATTACKABLE;
				md->min_chase=13;
			}
		}
	}
	return 0;
}

/*==========================================
 * The ?? routine of a link monster
 *------------------------------------------
 */
int mob_ai_sub_hard_linksearch(struct block_list &bl,va_list ap)
{
	struct mob_data &tmd = (struct mob_data &)bl;
	struct mob_data* md;
	struct block_list *target;

	nullpo_retr(0, ap);
	nullpo_retr(0, md=va_arg(ap,struct mob_data *));
	nullpo_retr(0, target=va_arg(ap,struct block_list *));

	if( md->attacked_id > 0 && mob_db[md->class_].mode&0x08){
		if (tmd.class_ == md->class_ && tmd.bl.m == md->bl.m && (!tmd.target_id || md->state.targettype == NONE_ATTACKABLE)){
			if( mob_can_reach(tmd,*target,12) ){	// Reachability judging
				tmd.target_id = md->attacked_id;
				md->attacked_count = 0;
				tmd.state.targettype = ATTACKABLE;
				tmd.min_chase=13;
			}
		}
	}

	return 0;
}
/*==========================================
 * Processing of slave monsters
 *------------------------------------------
 */
int mob_ai_sub_hard_slavemob(struct mob_data &md,unsigned long tick)
{
	struct mob_data *mmd=NULL;
	struct block_list *bl;
	int mode,race,old_dist;


	if((bl=map_id2bl(md.master_id)) != NULL )
		mmd=(struct mob_data *)bl;

	mode=mob_db[md.class_].mode;

	if(!mmd || mmd->hp <= 0)
	{	//�傪���S���Ă��邩������Ȃ�
		if(md.state.special_mob_ai>0)
			mob_timer_delete(0, 0, md.bl.id, 0);
		else
			mob_damage(md,md.hp,0,NULL);
		return 0;
	}
	if(md.state.special_mob_ai>0)		// �傪PC�̏ꍇ�́A�ȍ~�̏����͗v��Ȃ�
		return 0;

	// It is not main monster/leader.
	if(!mmd || mmd->bl.type != BL_MOB || mmd->bl.id != md.master_id)
		return 0;

	// �Ăі߂�
	if(mmd->state.recall_flag == 1){
		if (mmd->recallcount < (mmd->recallmob_count+2) ){
			mob_warp(md,-1,mmd->bl.x,mmd->bl.y,3);
			mmd->recallcount += 1;
		} else{
			mmd->state.recall_flag = 0;
			mmd->recallcount=0;
		}
		md.state.master_check = 1;
		return 0;
	}
	// Since it is in the map on which the master is not, teleport is carried out and it pursues.
	if( mmd->bl.m != md.bl.m ){
		mob_warp(md,mmd->bl.m,mmd->bl.x,mmd->bl.y,3);
		md.state.master_check = 1;
		return 0;
	}

	// Distance with between slave and master is measured.
	old_dist=md.master_dist;
	md.master_dist=distance(md.bl.x,md.bl.y,mmd->bl.x,mmd->bl.y);

	// Since the master was in near immediately before, teleport is carried out and it pursues.
	if( old_dist<10 && md.master_dist>18){
		mob_warp(md,-1,mmd->bl.x,mmd->bl.y,3);
		md.state.master_check = 1;
		return 0;
	}

	// Although there is the master, since it is somewhat far, it approaches.
	if((!md.target_id || md.state.targettype == NONE_ATTACKABLE) && mob_can_move(md) &&
		(md.walkpath.path_pos>=md.walkpath.path_len || md.walkpath.path_len==0) && md.master_dist<15){
		int i=0,dx,dy,ret;
		if(md.master_dist>4) {
			do {
				if(i<=5){
					dx=mmd->bl.x - md.bl.x;
					dy=mmd->bl.y - md.bl.y;
					if(dx<0) dx+=(rand()%( (dx<-3)?3:-dx )+1);
					else if(dx>0) dx-=(rand()%( (dx>3)?3:dx )+1);
					if(dy<0) dy+=(rand()%( (dy<-3)?3:-dy )+1);
					else if(dy>0) dy-=(rand()%( (dy>3)?3:dy )+1);
				}else{
					dx=mmd->bl.x - md.bl.x + rand()%7 - 3;
					dy=mmd->bl.y - md.bl.y + rand()%7 - 3;
				}

				ret=mob_walktoxy(md,md.bl.x+dx,md.bl.y+dy,0);
				i++;
			} while(ret && i<10);
		}
		else {
			do {
				dx = rand()%9 - 5;
				dy = rand()%9 - 5;
				if( dx == 0 && dy == 0) {
					dx = (rand()%1)? 1:-1;
					dy = (rand()%1)? 1:-1;
				}
				dx += mmd->bl.x;
				dy += mmd->bl.y;

				ret=mob_walktoxy(md,mmd->bl.x+dx,mmd->bl.y+dy,0);
				i++;
			} while(ret && i<10);
		}

		md.next_walktime=tick + 200+rand()%600;
		md.state.master_check = 1;
	}

	// There is the master, the master locks a target and he does not lock.
	if( (mmd->target_id>0 && mmd->state.targettype == ATTACKABLE) && (!md.target_id || md.state.targettype == NONE_ATTACKABLE) ){
		struct map_session_data *sd=map_id2sd(mmd->target_id);
		if(sd!=NULL && !pc_isdead(*sd) && sd->invincible_timer == -1 && !pc_isinvisible(*sd)){

			race=mob_db[md.class_].race;
			if(mode&0x20 ||
				(sd->sc_data[SC_TRICKDEAD].timer == -1 && sd->sc_data[SC_BASILICA].timer == -1 &&
				( (!pc_ishiding(*sd) && !sd->state.gangsterparadise) || ((race == 4 || race == 6 || mode&0x100) && !sd->state.perfect_hiding) ) ) ){	// �W�Q���Ȃ�������

				md.target_id=sd->bl.id;
				md.state.targettype = ATTACKABLE;
				md.min_chase=5+distance(md.bl.x,md.bl.y,sd->bl.x,sd->bl.y);
				md.state.master_check = 1;
			}
		}
	}

	// There is the master, the master locks a target and he does not lock.
/*	if( (md.target_id>0 && mmd->state.targettype == ATTACKABLE) && (!mmd->target_id || mmd->state.targettype == NONE_ATTACKABLE) ){
		struct map_session_data *sd=map_id2sd(md.target_id);
		if(sd!=NULL && !pc_isdead(sd) && sd->invincible_timer == -1 && !pc_isinvisible(sd)){

			race=mob_db[mmd->class_].race;
			if(mode&0x20 ||
				(sd->sc_data[SC_TRICKDEAD].timer == -1 &&
				(!(sd->status.option&0x06) || race==4 || race==6)
				) ){	// It judges whether there is any disturbance.

				mmd->target_id=sd->bl.id;
				mmd->state.targettype = ATTACKABLE;
				mmd->min_chase=5+distance(mmd->bl.x,mmd->bl.y,sd->bl.x,sd->bl.y);
			}
		}
	}*/

	return 0;
}

/*==========================================
 * A lock of target is stopped and mob moves to a standby state.
 *------------------------------------------
 */
int mob_unlocktarget(struct mob_data &md,unsigned long tick)
{
	md.target_id=0;
	md.state.targettype = NONE_ATTACKABLE;
	md.state.skillstate=MSS_IDLE;
	md.next_walktime=tick+rand()%3000+3000;
	return 0;
}
/*==========================================
 * Random walk
 *------------------------------------------
 */
int mob_randomwalk(struct mob_data &md,unsigned long tick)
{
	const int retrycount=20;
	int speed;

	speed=status_get_speed(&md.bl);
	if( DIFF_TICK(md.next_walktime,tick)<0 )
	{
		int i,x,y,c,d=12-md.move_fail_count;
		int mask[8][2] = {{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1},{1,0},{1,1}};
		if(d<5) d=5;
		for(i=0;i<retrycount;i++){	// Search of a movable place
			int r=rand();
			x=r%(d*2+1)-d;
			y=r/(d*2+1)%(d*2+1)-d;
			if (md.target_dir){
				if (x<0) x=0-x;
				if (y<0) y=0-y;
				x *= mask[md.target_dir-1][0];
				y *= mask[md.target_dir-1][1];
			}
			x+=md.bl.x;
			y+=md.bl.y;

			if((map_getcell(md.bl.m,x,y,CELL_CHKPASS)) && mob_walktoxy(md,x,y,1)==0){
				md.move_fail_count=0;
				break;
			}
			if(i+1>=retrycount){
				md.move_fail_count++;
				md.target_dir = 0;
				if(md.move_fail_count>1000){
					if(battle_config.error_log)
						ShowMessage("MOB cant move. random spawn %d, class_ = %d\n",md.bl.id,md.class_);
					md.move_fail_count=0;
					mob_spawn(md.bl.id);
				}
			}
		}
		for(i=c=0;i<md.walkpath.path_len;i++){	// The next walk start time is calculated.
			if(md.walkpath.path[i]&1)
				c+=speed*14/10;
			else
				c+=speed;
		}
		md.next_walktime = tick+rand()%3000+3000+c;
		md.state.skillstate=MSS_WALK;
		return 1;
	}
	return 0;
}

/*==========================================
 * AI of MOB whose is near a Player
 *------------------------------------------
 */
int mob_ai_sub_hard(struct block_list &bl,va_list ap)
{
	struct mob_data &md = (struct mob_data&)bl;
	struct mob_data *tmd = NULL;
	struct map_session_data *tsd = NULL;
	struct block_list *tbl = NULL;
	struct flooritem_data *fitem;
	unsigned long tick;
	int i, dx, dy, ret, dist;
	int attack_type = 0;
	int mode, race;
	int search_size = AREA_SIZE*2;
	int blind_flag = 0;

	nullpo_retr(0, ap);

	if(bl.type!=BL_MOB)
		return 0;

	tick=(unsigned long)va_arg(ap,int);

	if( DIFF_TICK(tick, md.last_thinktime) < MIN_MOBTHINKTIME )
		return 0;
	md.last_thinktime = tick;

	if (md.skilltimer != -1 || md.bl.prev == NULL ){	// Casting skill, or has died
		if( DIFF_TICK (tick, md.next_walktime) > MIN_MOBTHINKTIME )
			md.next_walktime = tick;
		return 0;
	}

	// Abnormalities
	if((md.opt1 > 0 && md.opt1 != 6) || md.state.state == MS_DELAY || md.sc_data[SC_BLADESTOP].timer != -1)
		return 0;

	if (md.sc_data && md.sc_data[SC_BLIND].timer != -1)
		blind_flag = 1;

	if (!md.mode)
		mode = mob_db[md.class_].mode;
	else
		mode = md.mode;
	race = mob_db[md.class_].race;

	if (!(mode & 0x80) && md.target_id > 0)
		md.target_id = 0;

	if( md.attacked_id > 0 && (mode&0x08) )
	{	// Link monster
		struct map_session_data *asd = map_id2sd (md.attacked_id);
		if (asd && !pc_isdead(*asd))
		{
			map_foreachinarea(mob_ai_sub_hard_linksearch, md.bl.m,
				((int)md.bl.x)-AREA_SIZE, ((int)md.bl.y)-AREA_SIZE,
				((int)md.bl.x)+AREA_SIZE, ((int)md.bl.y)+AREA_SIZE,
				BL_MOB, &md, &asd->bl);
		} else //If the target is not reachable, unlock it. [Skotlex]
			mob_unlocktarget(md, tick);
	}
	// It checks to see it was attacked first (if active, it is target change at 25% of probability).
	if( mode>0 &&
		md.attacked_id>0 &&
		(!md.target_id || md.state.targettype == NONE_ATTACKABLE || ( (mode&0x04) && rand()%100<25)) )
	{
		struct block_list *abl = map_id2bl(md.attacked_id);
		struct map_session_data *asd = NULL;

		if (abl)
		{
			dist = distance(md.bl.x, md.bl.y, abl->x, abl->y);

			if (abl->type == BL_PC)
				asd = (struct map_session_data *)abl;

			if (asd == NULL || md.bl.m != abl->m || abl->prev == NULL ||
				dist>= 32 ||
				battle_check_target(&bl, abl, BCT_ENEMY) == 0 ||
				!mob_can_reach(md, *abl, dist) )
			{
				md.attacked_id = 0;
//				if (md.attacked_count++ > 3)
// waiting for 3 hits until checking if fleeing is an option is stupid of the mob
				md.attacked_count++;
				{
					if( 0==mobskill_use(md, tick, MSC_RUDEATTACKED) &&
						(mode&1) )
					{
						static const int mask[8][2] = {{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1},{1,0},{1,1}};
						int dist = rand()%10 + 1;	//��ނ��鋗��
						int dir = map_calc_dir(*abl, bl.x, bl.y);

						if( mob_can_move(md) )
						{
							mob_walktoxy(md, md.bl.x + dist * mask[dir][0], md.bl.y + dist * mask[dir][1], 0);
							md.next_walktime = tick + 200+rand()%600;
						}
						else
						{	// mob is blocked, most likly by the damage delay time
							//!! add something to enable
							//!! the mob to flee away from the attacker
							//!! possibly a new state together with additional transitions
							md.to_x=md.bl.x + dist * mask[dir][0];
							md.to_y=md.bl.y + dist * mask[dir][1];
						}
						md.attacked_count = 0;// move this away later
					}
				}
			}
			else if (blind_flag && dist > 2 && DIFF_TICK(tick,md.next_walktime) < 0) {
				md.target_id = 0;
				md.attacked_id = 0;
				md.state.targettype = NONE_ATTACKABLE;
				if (mode & 1 && mob_can_move(md)) {
					dx = abl->x - md.bl.x;
					dy = abl->y - md.bl.y;
					md.next_walktime = tick + 1000;
					ret = mob_walktoxy(md, md.bl.x+dx, md.bl.y+dy, 0);
				}
			}
			else
			{	//�����������ꍇ�̓^�Q��ύX���Ȃ�
				if (!md.target_id || dist< 3 ) {
					md.target_id = md.attacked_id; // set target
					md.state.targettype = ATTACKABLE;
					attack_type = 1;
					md.attacked_id = md.attacked_count = 0;
					md.min_chase = dist + 13;
					if (md.min_chase > 26)
						md.min_chase = 26;
				}
			}
		}
	}

	md.state.master_check = 0;
	// Processing of slave monster
	if(md.master_id > 0)// && md.state.special_mob_ai == 0)
		mob_ai_sub_hard_slavemob(md, tick);

	// �A�N�e�B�������X�^�[�̍��G (?? of a bitter taste TIVU monster)
	if((!md.target_id || md.state.targettype == NONE_ATTACKABLE) && (mode&0x04) &&
		!md.state.master_check && battle_config.monster_active_enable) {
		i = 0;
		search_size = (blind_flag) ? 3 : AREA_SIZE*2;
		if (md.state.special_mob_ai)
			map_foreachinarea (mob_ai_sub_hard_activesearch, md.bl.m,
					((int)md.bl.x)-search_size, ((int)md.bl.y)-search_size,
					((int)md.bl.x)+search_size, ((int)md.bl.y)+search_size,
					0, &md, &i);
		else map_foreachinarea(mob_ai_sub_hard_activesearch, md.bl.m,
					((int)md.bl.x)-search_size,((int)md.bl.y)-search_size,
					((int)md.bl.x)+search_size,((int)md.bl.y)+search_size,
					BL_PC, &md, &i);
	}

	// The item search of a loot monster
	if (!md.target_id && (mode&0x02) && !md.state.master_check)
	{
		i = 0;
		search_size = (blind_flag) ? 3 : AREA_SIZE*2;
		map_foreachinarea (mob_ai_sub_hard_lootsearch, md.bl.m,
					((int)md.bl.x)-search_size, ((int)md.bl.y)-search_size,
					((int)md.bl.x)+search_size, ((int)md.bl.y)+search_size,
					BL_ITEM, &md, &i);
	}

	// It will attack, if the candidate for an attack is.
	if (md.target_id > 0)
	{
		if ((tbl = map_id2bl(md.target_id)))
		{
			if (tbl->type == BL_PC)
				tsd = (struct map_session_data *)tbl;
			else if(tbl->type == BL_MOB)
				tmd = (struct mob_data *)tbl;

			if(tsd || tmd)
			{	// pc or mob
				if( tbl->m != md.bl.m || tbl->prev == NULL ||
					(dist = distance(md.bl.x, md.bl.y, tbl->x, tbl->y)) >= search_size ||
					(tsd && pc_isdead(*tsd)) )
				{
					mob_unlocktarget(md,tick);	// �ʃ}�b�v���A���E�O
				}
				else if (blind_flag && dist > 2 && DIFF_TICK(tick,md.next_walktime) < 0)
				{
					md.target_id = 0;
					md.attacked_id = 0;
					md.state.targettype = NONE_ATTACKABLE;
					if (!(mode & 1) || !mob_can_move(md))
						return 0;
					dx = tbl->x - md.bl.x;
					dy = tbl->y - md.bl.y;
					md.next_walktime = tick + 800+rand()%400;
					ret = mob_walktoxy(md, md.bl.x+dx, md.bl.y+dy, 0);
				}
				else if( tsd && !(mode & 0x20) &&
					(tsd->sc_data[SC_TRICKDEAD].timer != -1 ||
					tsd->sc_data[SC_BASILICA].timer != -1 ||
					((pc_ishiding(*tsd) || tsd->state.gangsterparadise) &&
					!((race == 4 || race == 6 || mode&0x100) && !tsd->state.perfect_hiding))))
				{
					mob_unlocktarget(md,tick);	// �X�L���Ȃǂɂ����G�W�Q
				}
				else if (!battle_check_range (&md.bl, tbl, mob_db[md.class_].range))
				{	// �U���͈͊O�Ȃ̂ňړ�
					if(!(mode & 1))
					{	// �ړ����Ȃ����[�h
						mob_unlocktarget(md,tick);
						return 0;
					}
					if (!mob_can_move(md))	// �����Ȃ���Ԃɂ���
						return 0;
					md.state.skillstate = MSS_CHASE;	// �ˌ����X�L��
					mobskill_use (md, tick, -1);
					if (md.timer != -1 && md.state.state != MS_ATTACK &&
						(DIFF_TICK (md.next_walktime, tick) < 0 ||
						distance(md.to_x, md.to_y, tbl->x, tbl->y) < 2))
					{
						return 0; // ���Ɉړ���
					}
					search_size = (blind_flag) ? 3 : ((md.min_chase>13) ? md.min_chase : 13);
					if (!mob_can_reach(md, *tbl, search_size))
						mob_unlocktarget(md,tick);	// �ړ��ł��Ȃ��̂Ń^�Q�����iIW�Ƃ��H�j
					else
					{	// �ǐ�
						md.next_walktime = tick + 200+rand()%600;
						i = 0;
						do
						{
							if (i == 0)
							{	// �ŏ���AEGIS�Ɠ������@�Ō���
								dx = tbl->x - md.bl.x;
								dy = tbl->y - md.bl.y;
								if (dx < 0) dx++;
								else if (dx > 0) dx--;
								if (dy < 0) dy++;
								else if (dy > 0) dy--;
							}
							else
							{	// ���߂Ȃ�Athena��(�����_��)
								dx = tbl->x - md.bl.x + rand()%3 - 1;
								dy = tbl->y - md.bl.y + rand()%3 - 1;
							}
							ret = mob_walktoxy(md, md.bl.x + dx, md.bl.y + dy, 0);
							i++;
						} while (ret && i < 5);

						if (ret)
						{	// �ړ��s�\�ȏ�����̍U���Ȃ�2������
							if (dx < 0) dx = 2;
							else if (dx > 0) dx = -2;
							if (dy < 0) dy = 2;
							else if (dy > 0) dy = -2;
							mob_walktoxy (md, md.bl.x+dx, md.bl.y+dy, 0);
						}
					}
				}
				else
				{	// �U���˒��͈͓�
					md.state.skillstate = MSS_ATTACK;
					if (md.state.state == MS_WALK)
						mob_stop_walking(md, 1);	// ���s���Ȃ��~
					if (md.state.state == MS_ATTACK)
						return 0; // ���ɍU����
					mob_changestate(md, MS_ATTACK, attack_type);
				}
				return 0;
			}
			else
			{	// other target types
				// ���[�g�����X�^�[����
				if (tbl == NULL || tbl->type != BL_ITEM || tbl->m != md.bl.m ||
					(dist = distance(md.bl.x, md.bl.y, tbl->x, tbl->y)) >= md.min_chase || !md.lootitem ||
					(blind_flag && dist >= 4))
				{
					 // �������邩�A�C�e�����Ȃ��Ȃ���
					mob_unlocktarget (md, tick);
					if (md.state.state == MS_WALK)
						mob_stop_walking(md,1);	// ���s���Ȃ��~
				}
				else if (dist)
				{
					if (!(mode & 1))
					{	// �ړ����Ȃ����[�h
						mob_unlocktarget(md,tick);
						return 0;
					}
					if (!mob_can_move(md))	// �����Ȃ���Ԃɂ���
						return 0;
					md.state.skillstate = MSS_LOOT;	// ���[�g���X�L���g�p
					mobskill_use(md, tick, -1);
					if( md.timer != -1 && md.state.state != MS_ATTACK &&
						(DIFF_TICK(md.next_walktime,tick) < 0 ||
						 distance(md.to_x, md.to_y, tbl->x, tbl->y) <= 0) )
					{
						return 0; // ���Ɉړ���
					}
					md.next_walktime = tick + 500;
					dx = tbl->x - md.bl.x;
					dy = tbl->y - md.bl.y;
					ret = mob_walktoxy(md, md.bl.x+dx, md.bl.y+dy, 0);
					if (ret)
						mob_unlocktarget(md, tick);// �ړ��ł��Ȃ��̂Ń^�Q�����iIW�Ƃ��H�j
				}
				else
				{	// �A�C�e���܂ł��ǂ蒅����
					if (md.state.state == MS_ATTACK)
						return 0; // �U����
					if (md.state.state == MS_WALK)
						mob_stop_walking(md,1);	// ���s���Ȃ��~
					fitem = (struct flooritem_data *)tbl;
					if (md.lootitem_count < LOOTITEM_SIZE)
					{
						memcpy (&md.lootitem[md.lootitem_count++], &fitem->item_data, sizeof(md.lootitem[0]));
					}
					else if (battle_config.monster_loot_type == 1 && md.lootitem_count >= LOOTITEM_SIZE)
					{
						mob_unlocktarget(md,tick);
						return 0;
					}
					else
					{
						if (md.lootitem[0].card[0] == 0xff00)
							intif_delete_petdata( MakeDWord(md.lootitem[0].card[1],md.lootitem[0].card[2]) );
						for (i = 0; i < LOOTITEM_SIZE - 1; i++)
							memcpy (&md.lootitem[i], &md.lootitem[i+1], sizeof(md.lootitem[0]));
						memcpy (&md.lootitem[LOOTITEM_SIZE-1], &fitem->item_data, sizeof(md.lootitem[0]));
					}
					map_clearflooritem (tbl->id);
					mob_unlocktarget (md,tick);
				}
				return 0;
			}
		}
		else
		{
			mob_unlocktarget(md,tick);
			if (md.state.state == MS_WALK)
				mob_stop_walking(md,4);	// ���s���Ȃ��~
			return 0;
		}
	}

	// It is skill use at the time of /standby at the time of a walk.
	if( mobskill_use(md, tick, -1) )
		return 0;

	// ���s����
	if (mode & 1 && mob_can_move(md) &&	// �ړ��\MOB&�������Ԃɂ���
		(md.master_id == 0 || md.state.special_mob_ai || md.master_dist > 10))	//��芪��MOB����Ȃ�
	{
		if(DIFF_TICK(md.next_walktime, tick) > 7000 &&
			(md.walkpath.path_len == 0 || md.walkpath.path_pos >= md.walkpath.path_len))
			md.next_walktime = tick + 3000 * rand() % 2000;
		// Random movement
		if(mob_randomwalk(md,tick))
			return 0;
	}

	// Since he has finished walking, it stands by.
	if (md.walkpath.path_len == 0 || md.walkpath.path_pos >= md.walkpath.path_len)
		md.state.skillstate = MSS_IDLE;
	return 0;
}

/*==========================================
 * Serious processing for mob in PC field of view (foreachclient)
 *------------------------------------------
 */
int mob_ai_sub_foreachclient(struct map_session_data &sd,va_list ap)
{
	unsigned long tick;

	nullpo_retr(0, ap);
	tick=(unsigned long)va_arg(ap,int);

	map_foreachinarea(mob_ai_sub_hard,sd.bl.m,
					  ((int)sd.bl.x)-AREA_SIZE*2,((int)sd.bl.y)-AREA_SIZE*2,
					  ((int)sd.bl.x)+AREA_SIZE*2,((int)sd.bl.y)+AREA_SIZE*2,
					  BL_MOB,tick);
	return 0;
}

/*==========================================
 * Serious processing for mob in PC field of view   (interval timer function)
 *------------------------------------------
 */
int mob_ai_hard(int tid,unsigned long tick,int id,int data)
{
	clif_foreachclient(mob_ai_sub_foreachclient,tick);

	return 0;
}

/*==========================================
 * Negligent mode MOB AI (PC is not in near)
 *------------------------------------------
 */
int mob_ai_sub_lazy(void * key,void * data,va_list app)
{
	struct mob_data *md=(struct mob_data *)data;
	struct mob_data *mmd=NULL;
	unsigned long tick;
	va_list ap;

	if(NULL==md)
	{	int a=(int)key;
		ShowMessage("md NULL pointer, key=%i\n", a);
	}

	nullpo_retr(0, md);
	nullpo_retr(0, app);

	ap=va_arg(app,va_list);
	nullpo_retr(0, ap);

	if(md->bl.type!=BL_MOB)
		return 0;

	if (md->master_id > 0) {
		struct block_list *mbl = map_id2bl(md->master_id);
		if (mbl && mbl->type == BL_MOB)
			mmd = (struct mob_data *)mbl;	//������BOSS�̏��
	}

	tick=(unsigned long)va_arg(ap,int);

	if(DIFF_TICK(tick,md->last_thinktime)<MIN_MOBTHINKTIME*10)
		return 0;
	md->last_thinktime=tick;

	if(md->bl.prev==NULL || md->skilltimer!=-1){
		if(DIFF_TICK(tick,md->next_walktime)>MIN_MOBTHINKTIME*10)
			md->next_walktime=tick;
		return 0;
	}

	// ��芪�������X�^�[�̏����i�Ăі߂����ꂽ���j
	if(mmd && md->state.special_mob_ai == 0 && mmd->state.recall_flag == 1) {
		mob_ai_sub_hard_slavemob (*md,tick);
		return 0;
	}

	if( DIFF_TICK(md->next_walktime,tick)<0 &&
		(mob_db[md->class_].mode&1) && mob_can_move(*md) )
	{
		if( map[md->bl.m].users>0 )
		{	// Since PC is in the same map, somewhat better negligent processing is carried out.
			// It sometimes moves.
			if(rand()%1000<MOB_LAZYMOVEPERC)
				mob_randomwalk(*md,tick);

			// MOB which is not not the summons MOB but BOSS, either sometimes reboils.
			else if( rand()%1000<MOB_LAZYWARPPERC && md->master_id!=0 &&
				mob_db[md->class_].mexp <= 0 && !(mob_db[md->class_].mode & 0x20))
				mob_spawn(md->bl.id);
		}
		else
		{	// Since PC is not even in the same map, suitable processing is carried out even if it takes.
			// MOB which is not BOSS which is not Summons MOB, either -- a case -- sometimes -- leaping
			if( rand()%1000<MOB_LAZYWARPPERC && md->master_id!=0 &&
				mob_db[md->class_].mexp <= 0 && !(mob_db[md->class_].mode & 0x20))
				mob_warp(*md,-1,-1,-1,-1);
		}
		md->next_walktime = tick+rand()%10000+5000;
	}
	return 0;
}

/*==========================================
 * Negligent processing for mob outside PC field of view   (interval timer function)
 *------------------------------------------
 */
int mob_ai_lazy(int tid,unsigned long tick,int id,int data)
{
	map_foreachiddb(mob_ai_sub_lazy,tick);

	return 0;
}


/*==========================================
 * The structure object for item drop with delay
 * Since it is only two being able to pass [ int ] a timer function
 * Data is put in and passed to this structure object.
 *------------------------------------------
 */
struct delay_item_drop {
	unsigned short m;
	unsigned short x;
	unsigned short y;
	unsigned short nameid;
	unsigned short amount;
	struct map_session_data *first_sd,*second_sd,*third_sd;
};

struct delay_item_drop2 {
	int m,x,y;
	struct item item_data;
	struct map_session_data *first_sd,*second_sd,*third_sd;
};

/*==========================================
 * item drop with delay (timer function)
 *------------------------------------------
 */
int mob_delay_item_drop(int tid,unsigned long tick,int id,int data)
{
	struct delay_item_drop *ditem=(struct delay_item_drop *)id;
	struct item temp_item;
	int flag, drop_flag = 1;

	nullpo_retr(0, ditem);

	memset(&temp_item,0,sizeof(temp_item));
	temp_item.nameid = ditem->nameid;
	temp_item.amount = ditem->amount;
	temp_item.identify = !itemdb_isEquipment(temp_item.nameid);

	if(ditem->first_sd)
	{
#if 0
		if (ditem->first_sd->status.party_id > 0)
		{
			struct party *p;
			if((p=party_search(ditem->first_sd->status.party_id)) && p->item){
				struct map_session_data *sd = NULL;
				int i;
				for (i = p->itemc + 1; i!=p->itemc; i++)
				{	// initialise counter and loop through the party
					if (i >= MAX_PARTY)
						i = 0;	// reset counter to 1st person in party so it'll stop when it reaches "itemc"
					if ((sd=p->member[i].sd)!=NULL && sd->bl.m == ditem->first_sd->bl.m)
						break;
				}
				if (sd)
				{	// if an appropiate party member was found
					drop_flag = 0;
					if ((p->itemc++) >= MAX_PARTY)
						p->itemc = 0;
					if ((flag = pc_additem(*ditem->first_sd,temp_item,ditem->amount))) {
						clif_additem(ditem->first_sd,0,0,flag);
						drop_flag = 1;
					}
				}
			}
		}
		else
#endif
		if(battle_config.item_auto_get || ditem->first_sd->state.autoloot)
		{	//Autoloot added by Upa-Kun
			drop_flag = 0;
			if((flag = pc_additem(*ditem->first_sd,temp_item,ditem->amount)))
			{
				clif_additem(*ditem->first_sd,0,0,flag);
				drop_flag = 1;
			}
		}
	}
	if (drop_flag)
	{
		map_addflooritem(temp_item,1,ditem->m,ditem->x,ditem->y,ditem->first_sd,ditem->second_sd,ditem->third_sd,0);
	}
	aFree(ditem);
	return 0;
}

/*==========================================
 * item drop (timer function)-lootitem with delay
 *------------------------------------------
 */
int mob_delay_item_drop2(int tid,unsigned long tick,int id,int data)
{
	struct delay_item_drop2 *ditem=(struct delay_item_drop2 *)id;
	int flag, drop_flag = 1;

	nullpo_retr(0, ditem);

	if (ditem->first_sd)
	{
		#if 0
		if (ditem->first_sd->status.party_id > 0)
		{
			struct party *p;
			if((p=party_search(ditem->first_sd->status.party_id)) && p->item)
			{
				struct map_session_data *sd = NULL;
				int i;
				for (i = p->itemc + 1; i!=p->itemc; i++)
				{	// initialise counter and loop through the party
					if (i >= MAX_PARTY)
						i = 0;	// reset counter to 1st person in party so it'll stop when it reaches "itemc"
					if ((sd=p->member[i].sd)!=NULL && sd->bl.m == ditem->first_sd->bl.m)
						break;
				}
				if (sd)
				{	// if an appropiate party member was found
					drop_flag = 0;
					if ((p->itemc++) >= MAX_PARTY)
						p->itemc = 0;
					if((flag = pc_additem(*ditem->first_sd,ditem->item_data,ditem->item_data.amount)))
					{
						clif_additem(ditem->first_sd,0,0,flag);
						drop_flag = 1;
					}
				}
			}
		}
		else
		#endif
		if(battle_config.item_auto_get || ditem->first_sd->state.autoloot)
		{	//Autoloot added by Upa-Kun
			drop_flag = 0;
			if((flag = pc_additem(*ditem->first_sd,ditem->item_data,ditem->item_data.amount)))
			{
				clif_additem(*ditem->first_sd,0,0,flag);
				drop_flag = 1;
			}
			aFree(ditem);
			return 0;
		}
	}

	if (drop_flag) {
		map_addflooritem(ditem->item_data,ditem->item_data.amount,ditem->m,ditem->x,ditem->y,ditem->first_sd,ditem->second_sd,ditem->third_sd,0);
	}

	aFree(ditem);
	return 0;
}

/*==========================================
 * mob data is erased.
 *------------------------------------------
 */
int mob_remove_map(struct mob_data &md, int type)
{
	if( mob_get_viewclass(md.class_) <= 23 ||
		(mob_get_viewclass(md.class_) >= 4001 && mob_get_viewclass(md.class_) <= 4045))
		clif_clearchar_delay(gettick()+3000,md.bl,0);

	mob_changestate(md,MS_DEAD,0);
	mob_deleteslave(md);
	clif_clearchar_area(md.bl,type);
	map_delblock(md.bl);

	mob_setdelayspawn(md.bl.id);

	return 0;
}
void mob_unload(struct mob_data &md)
{
	mob_changestate(md,MS_DEAD,0);
	mob_deleteslave(md);
	clif_clearchar_area(md.bl, 0);
	map_delblock(md.bl);
	if(md.lootitem){
		aFree(md.lootitem);
		md.lootitem = NULL;
	}
	map_deliddb(md.bl);
	map_freeblock(&md);
}

int mob_timer_delete(int tid, unsigned long tick, int id, int data)
{
	struct mob_data *md=(struct mob_data *)map_id2bl(id);
	nullpo_retr(0, md);

	if( tid==md->deletetimer )
	{
		md->deletetimer = -1;
		// for Alchemist CANNIBALIZE [Lupus]
		mob_remove_map(*md, 3);
	}
	return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int mob_deleteslave_sub(struct block_list &bl,va_list ap)
{
	struct mob_data &md = (struct mob_data &)bl;
	unsigned long id;

	nullpo_retr(0, ap);
	id=va_arg(ap,int);

	if(bl.type==BL_MOB && md.master_id > 0 && md.master_id == id )
		mob_damage(md,md.hp,1,NULL);
	return 0;
}
/*==========================================
 *
 *------------------------------------------
 */
int mob_deleteslave(struct mob_data &md)
{
	if( md.state.is_master )
	{
		map_foreachinarea(mob_deleteslave_sub, md.bl.m,
			0,0,map[md.bl.m].xs-1,map[md.bl.m].ys-1,
			BL_MOB, md.bl.id);
	}
	return 0;
}

/*==========================================
 * It is the damage of sd to damage to md.
 *------------------------------------------
 */
int mob_damage(struct mob_data &md,int damage,int type,struct block_list *src)
{
	int i,count,minpos,mindmg;
	struct map_session_data *sd = NULL,*tmpsd[DAMAGELOG_SIZE];
	struct {
		struct party *p;
		unsigned long id;
		unsigned long base_exp;
		unsigned long job_exp;
		unsigned long zeny;
	} pt[DAMAGELOG_SIZE];
	int pnum=0;
	int mvp_damage,max_hp;
	unsigned long tick = gettick();
	struct map_session_data *mvp_sd = NULL, *second_sd = NULL,*third_sd = NULL;
	struct block_list *master = NULL;
	double tdmg,temp;
	struct item item;
	int ret;
	int drop_rate;
	int race;

	//src��NULL�ŌĂ΂��ꍇ������̂ŁA���Ń`�F�b�N

	max_hp = status_get_max_hp(&md.bl);
	race = status_get_race(&md.bl);

	if(src && src->type == BL_PC) {
		sd = (struct map_session_data *)src;
		mvp_sd = sd;
	}

//	if(battle_config.battle_log)
//		ShowMessage("mob_damage %d %d %d\n",md->hp,max_hp,damage);
	if(md.bl.prev==NULL){
		if(battle_config.error_log==1)
			ShowMessage("mob_damage : BlockError!!\n");
		return 0;
	}

	if(md.state.state==MS_DEAD || md.hp<=0) {
		if(md.bl.prev != NULL)
		{	// It is skill at the time of death.
			mobskill_use(md,tick,-1);
			mob_changestate(md,MS_DEAD,0);
			clif_clearchar_area(md.bl,1);
			map_delblock(md.bl);
			mob_setdelayspawn(md.bl.id);
		}
		return 0;
	}

	if(battle_config.monster_damage_delay && md.sc_data[SC_ENDURE].timer == -1)
		mob_stop_walking(md,3);
	if(damage > max_hp>>2)
		skill_stop_dancing(&md.bl,0);

	if(md.hp > max_hp)
		md.hp = max_hp;

	// The amount of overkill rounds to hp.
	if(damage>md.hp)
		damage=md.hp;

	if(!(type&2))
	{
		if(sd!=NULL)
		{
			for(i=0,minpos=0,mindmg=0x7fffffff;i<DAMAGELOG_SIZE;i++)
			{
				if(md.dmglog[i].fromid==sd->status.char_id)
					break;
				if(md.dmglog[i].fromid==0){
					minpos=i;
					mindmg=0;
				}
				else if(md.dmglog[i].dmg<mindmg){
					minpos=i;
					mindmg=md.dmglog[i].dmg;
				}
			}
			if(i<DAMAGELOG_SIZE)
				md.dmglog[i].dmg+=damage;
			else {
				//md.dmglog[minpos].id=sd->bl.id;
				md.dmglog[minpos].fromid=sd->status.char_id;
				md.dmglog[minpos].dmg=damage;
			}

			if(md.attacked_id <= 0 && md.state.special_mob_ai==0)
				md.attacked_id = sd->bl.id;
		}
		if(src && src->type == BL_PET && battle_config.pet_attack_exp_to_master==1)
		{
			struct pet_data *pd = (struct pet_data *)src;
			nullpo_retr(0, pd);
			for(i=0,minpos=0,mindmg=0x7fffffff;i<DAMAGELOG_SIZE;i++){
				//if(md->dmglog[i].id==pd->msd->bl.id)
				if(md.dmglog[i].fromid==pd->msd->status.char_id)
					break;
				if(md.dmglog[i].fromid==0){
					minpos=i;
					mindmg=0;
				}
				else if(md.dmglog[i].dmg<mindmg){
					minpos=i;
					mindmg=md.dmglog[i].dmg;
				}
			}
			if(i<DAMAGELOG_SIZE)
				md.dmglog[i].dmg+=(damage*battle_config.pet_attack_exp_rate)/100;
			else {
				//md.dmglog[minpos].id=pd->msd->bl.id;
				md.dmglog[minpos].fromid=pd->msd->status.char_id;
				md.dmglog[minpos].dmg=(damage*battle_config.pet_attack_exp_rate)/100;
			}
		}
		if(src && src->type == BL_MOB && ((struct mob_data*)src)->state.special_mob_ai){
			struct mob_data *md2 = (struct mob_data *)src;
			struct map_session_data *msd = map_id2sd(md2->master_id);
			nullpo_retr(0, md2);
			nullpo_retr(0, msd);
			for(i=0,minpos=0,mindmg=0x7fffffff;i<DAMAGELOG_SIZE;i++){
				if(md.dmglog[i].fromid==msd->status.char_id)
					break;
				if(md.dmglog[i].fromid==0){
					minpos=i;
					mindmg=0;
				}
				else if(md.dmglog[i].dmg<mindmg){
					minpos=i;
					mindmg=md.dmglog[i].dmg;
				}
			}
			if(i<DAMAGELOG_SIZE)
				md.dmglog[i].dmg+=damage;
			else {
				md.dmglog[minpos].fromid=msd->status.char_id;;
				md.dmglog[minpos].dmg=damage;

			if(md.attacked_id <= 0 && md.state.special_mob_ai==0)
				md.attacked_id = md2->master_id;
			}
		}
	}

	if(md.hp>damage)
		md.hp-=damage;
	else
		md.hp = 0;

	if(md.class_ >= 1285 && md.class_ <=1287) {	// guardian hp update [Valaris]
		struct guild_castle *gc=guild_mapname2gc(map[md.bl.m].mapname);
		if(gc) {

				if(md.bl.id==gc->GID0) {
					gc->Ghp0=md.hp;
					if(gc->Ghp0<=0) {
					        guild_castledatasave(gc->castle_id,10,0);
						guild_castledatasave(gc->castle_id,18,0);
					}
				}
				if(md.bl.id==gc->GID1) {
					gc->Ghp1=md.hp;
					if(gc->Ghp1<=0) {
					        guild_castledatasave(gc->castle_id,11,0);
						guild_castledatasave(gc->castle_id,19,0);
					}
				}
				if(md.bl.id==gc->GID2) {
					gc->Ghp2=md.hp;
					if(gc->Ghp2<=0) {
					        guild_castledatasave(gc->castle_id,12,0);
						guild_castledatasave(gc->castle_id,20,0);
					}
				}
				if(md.bl.id==gc->GID3) {
					gc->Ghp3=md.hp;
					if(gc->Ghp3<=0) {
					        guild_castledatasave(gc->castle_id,13,0);
						guild_castledatasave(gc->castle_id,21,0);
					}
				}
				if(md.bl.id==gc->GID4) {
					gc->Ghp4=md.hp;
					if(gc->Ghp4<=0) {
					        guild_castledatasave(gc->castle_id,14,0);
						guild_castledatasave(gc->castle_id,22,0);
					}
				}
				if(md.bl.id==gc->GID5) {
					gc->Ghp5=md.hp;
					if(gc->Ghp5<=0) {
					        guild_castledatasave(gc->castle_id,15,0);
						guild_castledatasave(gc->castle_id,23,0);
					}
				}
				if(md.bl.id==gc->GID6) {
					gc->Ghp6=md.hp;
					if(gc->Ghp6<=0) {
					        guild_castledatasave(gc->castle_id,16,0);
						guild_castledatasave(gc->castle_id,24,0);
					}
				}
				if(md.bl.id==gc->GID7) {
					gc->Ghp7=md.hp;
					if(gc->Ghp7<=0) {
					        guild_castledatasave(gc->castle_id,17,0);
						guild_castledatasave(gc->castle_id,25,0);

					}
				}
		}
	}	// end addition [Valaris]

	if(md.option&2 )
		status_change_end(&md.bl, SC_HIDING, -1);
	if(md.option&4 )
		status_change_end(&md.bl, SC_CLOAKING, -1);

	if( md.state.special_mob_ai == 2 &&
		src && md.master_id == src->id )
	{	//�X�t�B�A�[�}�C��

		md.state.alchemist = 1;
		md.target_dir = map_calc_dir(*src,md.bl.x,md.bl.y)+1;
		mobskill_use(md, tick, MSC_ALCHEMIST);
	}

	if(md.hp>0){
		if (battle_config.show_mob_hp)
			clif_update_mobhp (md);
		return 0;
	}

	// ----- �������玀�S���� -----

	map_freeblock_lock();
	mobskill_use(md,tick,-1);	// ���S���X�L��
	mob_changestate(md,MS_DEAD,0);

	memset(tmpsd,0,sizeof(tmpsd));
	memset(pt,0,sizeof(pt));

	max_hp = status_get_max_hp(&md.bl);

	if(src && src->type == BL_MOB)
		mob_unlocktarget(*((struct mob_data *)src),tick);


	if(sd) {
		int sp = 0, hp = 0;
		if(src && sd->state.attack_type == BF_MAGIC && (i=pc_checkskill(*sd,HW_SOULDRAIN))>0){	/* �\�E���h���C�� */
			clif_skill_nodamage(*src,md.bl,HW_SOULDRAIN,i,1);
			sp += (status_get_lv(&md.bl))*(65+15*i)/100;
		}
		sp += sd->sp_gain_value;
		sp += sd->sp_gain_race[race];
		hp += sd->hp_gain_value;
		if (sp > 0) {
			if(sd->status.sp + sp > sd->status.max_sp)
				sp = sd->status.max_sp - sd->status.sp;
			sd->status.sp += sp;
			if (sp > 0 && battle_config.show_hp_sp_gain)
				clif_heal(sd->fd,SP_SP,sp);
		}
		if (hp > 0) {
			if(sd->status.hp + hp > sd->status.max_hp)
				hp = sd->status.max_hp - sd->status.hp;
			sd->status.hp += hp;
			if (hp > 0 && battle_config.show_hp_sp_gain)
				clif_heal(sd->fd,SP_HP,hp);
		}
	}

	// map�O�ɏ������l�͌v�Z���珜���̂�
	// overkill���͖�������sum��max_hp�Ƃ͈Ⴄ

	map_session_data *damager[DAMAGELOG_SIZE];
	int dmcount=0;

	tdmg = 0;
	for(i=0,count=0,mvp_damage=0;i<DAMAGELOG_SIZE;i++){
		if(md.dmglog[i].fromid==0)
			continue;

		tmpsd[i] = map_charid2sd(md.dmglog[i].fromid);
		if(tmpsd[i] == NULL)
			continue;

		count++;
		if(tmpsd[i]->bl.m != md.bl.m || pc_isdead(*tmpsd[i]))
			continue;

		// make a list of all that attackers
		damager[dmcount++] = tmpsd[i];

		tdmg += (double)md.dmglog[i].dmg;
		if(mvp_damage<md.dmglog[i].dmg){
			third_sd = second_sd;
			second_sd = mvp_sd;
			mvp_sd=tmpsd[i];
			mvp_damage=md.dmglog[i].dmg;
		}
	}
	if(mvp_sd)
	{	// kill steal karma code
		for(i=0; i<dmcount; i++)
		{	// the mvp_sd is the reference
			if( damager[i] && damager[i] != mvp_sd && damager[i])
			{
				if( damager[i]->status.party_id == mvp_sd->status.party_id ||
					damager[i]->status.guild_id == mvp_sd->status.guild_id )
				{	// same guild or party
					if( damager[i]->status.karma>-100 && rand()%1000<  1 )	//  0.1% chance
						damager[i]->status.karma--; // honour points earned
				}
				else
				{	// possibly a killstealer
					if( i>0 && damager[i]->status.karma< 100 && rand()%1000<100 )	// 10.0% chance
						damager[i]->status.karma++;	// honour points lost
				}
			}
		}
	}


	// [MouseJstr]
	if((map[md.bl.m].flag.pvp == 0) || (battle_config.pvp_exp == 1)) {

	// �o���l�̕��z
	for(i=0;i<DAMAGELOG_SIZE;i++){
		unsigned short pid,base_exp,job_exp;
		int flag=1,zeny=0;
		double per;
		struct party *p;
		if(tmpsd[i]==NULL || tmpsd[i]->bl.m != md.bl.m || pc_isdead(*tmpsd[i]))
			continue;

		if (battle_config.exp_calc_type == 0) {
			// jAthena's exp formula
			per = ((double)md.dmglog[i].dmg)*(9.+(double)((count > 6)? 6:count))/10./tdmg;
			temp = (double)mob_db[md.class_].base_exp * per;
			base_exp = (temp > 2147483647.) ? 0x7fffffff : (int)temp;
			temp = (double)mob_db[md.class_].job_exp * per;
			job_exp = (temp > 2147483647.) ? 0x7fffffff : (int)temp;
		}
		else if (battle_config.exp_calc_type == 1) {
			//eAthena's exp formula rather than jAthena's
			per = (double)md.dmglog[i].dmg*256*(9+(double)((count > 6)? 6:count))/10/(double)max_hp;
			if (per > 512) per = 512;
			if (per < 1) per = 1;
			temp = (double)(mob_db[md.class_].base_exp*per/256);
			base_exp = (temp > 2147483647.) ? 0x7fffffff : (int)temp;
			temp = (double)(mob_db[md.class_].job_exp*per/256);
			job_exp = (temp > 2147483647.) ? 0x7fffffff : (int)temp;
		}
		else {
			//eAthena's exp formula rather than jAthena's, but based on total damage dealt
			per = (double)md.dmglog[i].dmg*256*(9+(double)((count > 6)? 6:count))/10/tdmg;
			if (per > 512) per = 512;
			if (per < 1) per = 1;
			temp = (double)(mob_db[md.class_].base_exp*per/256);
			base_exp = (temp > 2147483647.) ? 0x7fffffff : (int)temp;
			temp = (double)(mob_db[md.class_].job_exp*per/256);
			job_exp = (temp > 2147483647.) ? 0x7fffffff : (int)temp;
		}

		if (base_exp < 1) base_exp = 1;
		if (job_exp < 1) job_exp = 1;

		if(sd) {
			int rate;
			if ((rate = sd->expaddrace[race]) > 0) {
				base_exp += base_exp* rate/100;
				job_exp  += job_exp * rate/100;
			}
			if (battle_config.pk_mode && (mob_db[md.class_].lv - sd->status.base_level >= 20)) {
				// pk_mode additional exp if monster >20 levels [Valaris]
				base_exp += base_exp* 15/100;
				job_exp  += job_exp * 15/100;
			}
		}
		if(md.state.size==1) { // change experience for different sized monsters [Valaris]
			if(base_exp > 1)	base_exp/=2;
			if(job_exp > 1)		job_exp/=2;
		}
		else if(md.state.size==2) {
			base_exp*=2;
			job_exp*=2;
		}
		if(md.master_id) {
			if(((master = map_id2bl(md.master_id)) && status_get_mode(master)&0x20) ||	// check if its master is a boss (MVP's and minibosses)
				(md.state.special_mob_ai >= 1 && battle_config.alchemist_summon_reward != 1)) { // for summoned creatures [Valaris]
				base_exp = 0;
				job_exp = 0;
			}
		} else {
			if(battle_config.zeny_from_mobs) {
				if(md.level > 0) zeny=(int) ((md.level+rand()%md.level)*per/256); // zeny calculation moblv + random moblv [Valaris]
				if(mob_db[md.class_].mexp > 0)
					zeny*=rand()%250;
				// change zeny for different sized monsters [Valaris]
				if(md.state.size==1 && zeny >=2)
					zeny/=2;
				else if(md.state.size==2)
					zeny*=2;
			}
			if(battle_config.mobs_level_up && md.level > mob_db[md.class_].lv) { // [Valaris]
				job_exp+=(int) (((md.level-mob_db[md.class_].lv)*mob_db[md.class_].job_exp*.03)*per/256);
				base_exp+=(int) (((md.level-mob_db[md.class_].lv)*mob_db[md.class_].base_exp*.03)*per/256);
			}
		}
		//mapflags: noexp check [Lorky]
		if (map[md.bl.m].flag.nobaseexp == 1)	base_exp=0;
		if (map[md.bl.m].flag.nojobexp == 1)	job_exp=0;
		//end added Lorky
		if((pid=tmpsd[i]->status.party_id)>0){	// �p�[�e�B�ɓ����Ă���
			int j;
			for(j=0;j<pnum;j++)	// �����p�[�e�B���X�g�ɂ��邩�ǂ���
				if(pt[j].id==pid)
					break;
			if(j==pnum){	// ���Ȃ��Ƃ��͌������ǂ����m�F
				if((p=party_search(pid))!=NULL && p->expshare==1){
					pt[pnum].id=pid;
					pt[pnum].p=p;
					pt[pnum].base_exp=base_exp;
					pt[pnum].job_exp=job_exp;
					if(battle_config.zeny_from_mobs)
						pt[pnum].zeny=zeny; // zeny share [Valaris]
					pnum++;
					flag=0;
				}
			}else{	// ����Ƃ��͌���
				pt[j].base_exp+=base_exp;
				pt[j].job_exp+=job_exp;
				if(battle_config.zeny_from_mobs)
					pt[j].zeny+=zeny;  // zeny share [Valaris]
				flag=0;
			}
		}
		if(flag) {	// added zeny from mobs [Valaris]
			if(base_exp > 0 || job_exp > 0)
			pc_gainexp(*tmpsd[i],base_exp,job_exp);
			if (battle_config.zeny_from_mobs && zeny > 0) {
				pc_getzeny(*tmpsd[i],zeny); // zeny from mobs [Valaris]
			}
		}

	}
	// �������z
	for(i=0;i<pnum;i++)
		if(pt[i].p) party_exp_share(*pt[i].p,md.bl.m,pt[i].base_exp,pt[i].job_exp,pt[i].zeny);

	// item drop
	if (!(type&1)) {
		int log_item[10] = {0}; //8 -> 10 Lupus
		int drop_ore = -1, drop_items = 0; //slot N for DROP LOG, number of dropped items
		for (i = 0; i < 10; i++) { // 8 -> 10 Lupus
			struct delay_item_drop *ditem;
			int drop_rate;

			if ((master && status_get_mode(master) & 0x20) ||	// check if its master is a boss (MVP's and minibosses)
				(md.state.special_mob_ai >= 1 && battle_config.alchemist_summon_reward != 1))	// Added [Valaris]
				break;	// End
			if (mob_db[md.class_].dropitem[i].nameid <= 0)
				continue;
			drop_rate = mob_db[md.class_].dropitem[i].p;
			// change drops depending on monsters size [Valaris]
			if(md.state.size==1)			drop_rate/=2;
			else if(md.state.size==2)		drop_rate*=2;
			if (drop_rate <= 0 && !battle_config.drop_rate0item)
				drop_rate = 1;

			//Drops affected by luk as a % increase [Skotlex] (original implementation by Valaris)
			if (src && battle_config.drops_by_luk > 0)
				drop_rate += drop_rate*status_get_luk(src)*battle_config.drops_by_luk/10000;
			if (sd && battle_config.pk_mode == 1 && (mob_db[md.class_].lv - sd->status.base_level >= 20))
				drop_rate += drop_rate/4; // pk_mode increase drops if 20 level difference [Valaris]

			//mapflag: noloot check [Lorky]
			if (map[md.bl.m].flag.nomobloot == 1)	drop_rate=0;
			//end added [Lorky]

			if (drop_rate < rand() % 10000 + 1) { //fixed 0.01% impossible drops bug [Lupus]
				drop_ore = i; //we remember an empty slot to put there ORE DISCOVERY drop later.
				continue;
			}
			drop_items++; //we count if there were any drops

			ditem = (struct delay_item_drop *)aCalloc(1,sizeof(struct delay_item_drop));
			ditem->nameid = mob_db[md.class_].dropitem[i].nameid;
			log_item[i] = ditem->nameid;
			ditem->amount = 1;
			ditem->m = md.bl.m;
			ditem->x = md.bl.x;
			ditem->y = md.bl.y;
			ditem->first_sd = mvp_sd;
			ditem->second_sd = second_sd;
			ditem->third_sd = third_sd;
			add_timer(tick+500+i,mob_delay_item_drop,(int)ditem,0); //!!todo!!
		}

		// Ore Discovery [Celest]
		if(sd && sd == mvp_sd && map[md.bl.m].flag.nomobloot==0 && pc_checkskill(*sd,BS_FINDINGORE)>0 && battle_config.finding_ore_rate/100 >= (unsigned long)(rand()%1000))
		{
			struct delay_item_drop *ditem;
			ditem=(struct delay_item_drop *)aCalloc(1,sizeof(struct delay_item_drop));
			ditem->nameid = itemdb_searchrandomid(6);
			if (drop_ore<0) i=8; //we have only 10 slots in LOG, there's a check to not overflow (9th item usually a card, so we use 8th slot)
			log_item[i] = ditem->nameid; //it's for logging only
			drop_items++; //we count if there were any drops
			ditem->amount = 1;
			ditem->m = md.bl.m;
			ditem->x = md.bl.x;
			ditem->y = md.bl.y;
			ditem->first_sd = mvp_sd;
			ditem->second_sd = second_sd;
			ditem->third_sd = third_sd;
			add_timer(tick+500+i,mob_delay_item_drop,(int)ditem,0); //!!todo!!
		}

		//this drop log contains ALL dropped items + ORE (if there was ORE Recovery) [Lupus]
		if(sd && log_config.drop > 0 && drop_items) //we check were there any drops.. and if not - don't write the log
			log_drop(*sd, md.class_, log_item); //mvp_sd

		if(sd && sd->state.attack_type == BF_WEAPON) {
			int itemid = 0;
			for (i = 0; i < sd->monster_drop_item_count; i++) {
				struct delay_item_drop *ditem;
				if (sd->monster_drop_itemid[i] < 0)
					continue;
				if (sd->monster_drop_race[i] & (1<<race) ||
					(mob_db[md.class_].mode & 0x20 && sd->monster_drop_race[i] & 1<<10) ||
					(!(mob_db[md.class_].mode & 0x20) && sd->monster_drop_race[i] & 1<<11) )
				{
					if (sd->monster_drop_itemrate[i] <= rand()%10000+1)
						continue;
					itemid = (sd->monster_drop_itemid[i] > 0) ? sd->monster_drop_itemid[i] :
						itemdb_searchrandomgroup(sd->monster_drop_itemgroup[i]);

					ditem=(struct delay_item_drop *)aCalloc(1,sizeof(struct delay_item_drop));
					ditem->nameid = itemid;
					ditem->amount = 1;
					ditem->m = md.bl.m;
					ditem->x = md.bl.x;
					ditem->y = md.bl.y;
					ditem->first_sd = mvp_sd;
					ditem->second_sd = second_sd;
					ditem->third_sd = third_sd;
					add_timer(tick+520+i,mob_delay_item_drop,(int)ditem,0); //!!todo!!
				}
			}
			if(sd->get_zeny_num > 0)
				pc_getzeny(*sd,mob_db[md.class_].lv*10 + rand()%(sd->get_zeny_num+1));
		}
		if(md.lootitem) {
			for(i=0;i<md.lootitem_count;i++) {
				struct delay_item_drop2 *ditem;

				ditem=(struct delay_item_drop2 *)aCalloc(1,sizeof(struct delay_item_drop2));
				memcpy(&ditem->item_data,&md.lootitem[i],sizeof(struct item));
				ditem->m = md.bl.m;
				ditem->x = md.bl.x;
				ditem->y = md.bl.y;
				ditem->first_sd = mvp_sd;
				ditem->second_sd = second_sd;
				ditem->third_sd = third_sd;
				add_timer(tick+540+i,mob_delay_item_drop2,(int)ditem,0);//!!todo!!
			}
		}
	}

	// mvp����
	if(mvp_sd && mob_db[md.class_].mexp > 0 && !md.state.special_mob_ai){
		int log_mvp[2] = {0};
		int j;
		int mexp;
		temp = ((double)mob_db[md.class_].mexp * (9.+(double)count)/10.);	//[Gengar]
		mexp = (temp > 2147483647.)? 0x7fffffff:(int)temp;

		//mapflag: noexp check [Lorky]
		if (map[md.bl.m].flag.nobaseexp == 1 || map[md.bl.m].flag.nojobexp == 1)	mexp=1;
		//end added [Lorky]

		if(mexp < 1) mexp = 1;
		clif_mvp_effect(*mvp_sd);					// �G�t�F�N�g
		clif_mvp_exp(*mvp_sd,mexp);
		pc_gainexp(*mvp_sd,mexp,0);
		log_mvp[1] = mexp;
		for(j=0;j<3;j++){
			i = rand() % 3;
			if(mob_db[md.class_].mvpitem[i].nameid <= 0)
				continue;
			drop_rate = mob_db[md.class_].mvpitem[i].p;
			if(drop_rate <= 0 && !battle_config.drop_rate0item)
				drop_rate = 1;
/*			if(drop_rate < battle_config.item_drop_mvp_min)
				drop_rate = battle_config.item_drop_mvp_min;
			else if(drop_rate > battle_config.item_drop_mvp_max) //fixed
				drop_rate = battle_config.item_drop_mvp_max;
*/
			//mapflag: noloot check [Lorky]
			if (map[md.bl.m].flag.nomvploot == 1)	drop_rate=0;
			//end added Lorky

			if(drop_rate <= rand()%10000+1) //if ==0, then it doesn't drop
				continue;
			memset(&item,0,sizeof(item));
			item.nameid=mob_db[md.class_].mvpitem[i].nameid;
			item.identify = !itemdb_isEquipment(item.nameid);
			clif_mvp_item(*mvp_sd,item.nameid);
			log_mvp[0] = item.nameid;
			if(mvp_sd->weight*2 > mvp_sd->max_weight)
				map_addflooritem(item,1,mvp_sd->bl.m,mvp_sd->bl.x,mvp_sd->bl.y,mvp_sd,second_sd,third_sd,1);
			else if((ret = pc_additem(*mvp_sd,item,1))) {
				clif_additem(*sd,0,0,ret);
				map_addflooritem(item,1,mvp_sd->bl.m,mvp_sd->bl.x,mvp_sd->bl.y,mvp_sd,second_sd,third_sd,1);
			}
			break;
		}

		if(log_config.mvpdrop > 0)
			log_mvpdrop(*mvp_sd, md.class_, log_mvp);
	}

        } // [MouseJstr]

	// <Agit> NPC Event [OnAgitBreak]
	if(md.npc_event[0] && strcmp(((md.npc_event)+strlen(md.npc_event)-13),"::OnAgitBreak") == 0) {
		ShowMessage("MOB.C: Run NPC_Event[OnAgitBreak].\n");
		if (agit_flag == 1) //Call to Run NPC_Event[OnAgitBreak]
			guild_agit_break(md);
	}

		// SCRIPT���s
	if(md.npc_event[0])
	{
//		if(battle_config.battle_log)
//			ShowMessage("mob_damage : run event : %s\n",md->npc_event);
		if(src && src->type == BL_PET)
			sd = ((struct pet_data *)src)->msd;
		if(sd == NULL) {
			if(mvp_sd != NULL)
				sd = mvp_sd;
			else {
				struct map_session_data *tmpsd;
				size_t i;
				for(i=0;i<fd_max;i++){
					if(session[i] && (tmpsd= (struct map_session_data *) session[i]->session_data) && tmpsd->state.auth) {
						if(md.bl.m == tmpsd->bl.m) {
							sd = tmpsd;
							break;
						}
					}
				}
			}
		}
		if(sd)
			npc_event(*sd,md.npc_event,0);
	}
	//lordalfa
	else if (mvp_sd)
	{
		pc_setglobalreg(*mvp_sd,"killedrid",(md.class_));
		if (script_config.event_script_type == 0)
		{
			struct npc_data *npc= npc_name2id("NPCKillEvent");
			if(npc && npc->u.scr.ref)
			{
				run_script(npc->u.scr.ref->script,0,mvp_sd->bl.id,npc->bl.id); // NPCKillNPC
				ShowStatus("Event '"CL_WHITE"NPCKillEvent"CL_RESET"' executed.\n");
			}
		}
		else
		{
			ShowStatus("%d '"CL_WHITE"%s"CL_RESET"' events executed.\n",
				npc_event_doall_id("NPCKillEvent", mvp_sd->bl.id), "NPCKillEvent");
		}
	}
	//lordalfa

	if(battle_config.mob_clear_delay)
		clif_clearchar_delay(tick+battle_config.mob_clear_delay,md.bl,1);
	else
		clif_clearchar_area(md.bl,1);

	// reset levels for reborn moc
	md.level=0;


	mob_remove_map(md, 0);
	map_freeblock_unlock();

	return 0;
}

/*==========================================
 *------------------------------------------
 */
int mob_class_change(struct mob_data &md, int value[], size_t count)
{
	unsigned long tick = gettick();
	int i,c,hp_rate,max_hp,class_;

	nullpo_retr(0, value);
	if(md.bl.prev == NULL) return 0;

	if(count==0)
	{	// no count specified, look into the array manually, but take only max 5 elements
		while(count < 5 && value[count] > 1000 && value[count] <= MAX_MOB_DB) count++;
		if(count < 1)	// nothing found
			return 0;
	}
	else
	{	// check if at least the first value is valid
		if(value[0] <= 1000 || value[0] > MAX_MOB_DB)
			return 0;
	}

	class_ = value[rand()%count];
	if(class_<=1000 || class_>MAX_MOB_DB) class_ = value[0];

	max_hp = status_get_max_hp(&md.bl);
	hp_rate = md.hp*100/max_hp;
	clif_mob_class_change(md,class_);
	md.class_ = class_;
	max_hp = status_get_max_hp(&md.bl);
	if (battle_config.monster_class_change_full_recover) {
		md.hp = max_hp;
		memset(md.dmglog,0,sizeof(md.dmglog));
	} else
		md.hp = max_hp*hp_rate/100;

	if(md.hp > max_hp)
		md.hp = max_hp;
	else if(md.hp < 1)
		md.hp = 1;

	memcpy(md.name,mob_db[class_].jname,24);

	memset(&md.state,0,sizeof(md.state));
	md.attacked_id = 0;
	md.target_id = 0;
	md.move_fail_count = 0;

	md.speed = mob_db[md.class_].speed;
	md.def_ele = mob_db[md.class_].element;

	mob_changestate(md,MS_IDLE,0);
	skill_castcancel(&md.bl,0);
	md.state.skillstate = MSS_IDLE;
	md.last_thinktime = tick;
	md.next_walktime = tick+rand()%50+5000;
	md.attackabletime = tick;
	md.canmove_tick = tick;

	for(i=0,c=tick-1000*3600*10;i<MAX_MOBSKILL;i++)
		md.skilldelay[i] = c;
	md.skillid=0;
	md.skilllv=0;

	if(md.lootitem == NULL && mob_db[class_].mode&0x02)
		md.lootitem=(struct item *)aCalloc(LOOTITEM_SIZE,sizeof(struct item));

	skill_clear_unitgroup(&md.bl);
	skill_cleartimerskill(&md.bl);

	clif_clearchar_area(md.bl,0);
	clif_spawnmob(md);

	return 0;
}

/*==========================================
 * mob��
 *------------------------------------------
 */
int mob_heal(struct mob_data &md,int heal)
{
	int max_hp;

	max_hp = status_get_max_hp(&md.bl);

	md.hp += heal;
	if( max_hp < md.hp )
		md.hp = max_hp;

	if(md.class_ >= 1285 && md.class_ <=1287) {	// guardian hp update [Valaris]
		struct guild_castle *gc=guild_mapname2gc(map[md.bl.m].mapname);
		if(gc) {
			if(md.bl.id==gc->GID0) gc->Ghp0=md.hp;
			if(md.bl.id==gc->GID1) gc->Ghp1=md.hp;
			if(md.bl.id==gc->GID2) gc->Ghp2=md.hp;
			if(md.bl.id==gc->GID3) gc->Ghp3=md.hp;
			if(md.bl.id==gc->GID4) gc->Ghp4=md.hp;
			if(md.bl.id==gc->GID5) gc->Ghp5=md.hp;
			if(md.bl.id==gc->GID6) gc->Ghp6=md.hp;
			if(md.bl.id==gc->GID7) gc->Ghp7=md.hp;
		}
	}	// end addition [Valaris]

	if (battle_config.show_mob_hp)
		clif_update_mobhp(md);

	return 0;
}


/*==========================================
 * Added by RoVeRT
 *------------------------------------------
 */
int mob_warpslave_sub(struct block_list &bl,va_list ap)
{
	struct mob_data &md=(struct mob_data &)bl;
	unsigned long id,x,y;
	id=va_arg(ap,unsigned long);
	x=va_arg(ap,unsigned long);
	y=va_arg(ap,unsigned long);
	if( md.master_id==id ) {
		mob_warp(md,-1,x,y,2);
	}
	return 0;
}

/*==========================================
 * Added by RoVeRT
 *------------------------------------------
 */
int mob_warpslave(struct mob_data &md,int x, int y)
{
//ShowMessage("warp slave\n");
	map_foreachinarea(mob_warpslave_sub, md.bl.m,
		x-AREA_SIZE,y-AREA_SIZE,
		x+AREA_SIZE,y+AREA_SIZE,BL_MOB,
		md.bl.id, md.bl.x, md.bl.y );
	return 0;
}

/*==========================================
 * mob���[�v
 *------------------------------------------
 */
int mob_warp(struct mob_data &md,int m,int x,int y,int type)
{
	int i=0,xs=0,ys=0,bx=x,by=y;
	unsigned long tick = gettick();


	if( md.bl.prev==NULL )
		return 0;

	if( m<0 || (size_t)m>=map_num ) m=md.bl.m;

	if(type >= 0) {
		if(map[md.bl.m].flag.monster_noteleport)
			return 0;
		clif_clearchar_area(md.bl,type);
	}
	skill_unit_move(md.bl,tick,0);
	map_delblock(md.bl);

	if(bx>0 && by>0){	// �ʒu�w��̏ꍇ���͂X�Z����T��
		xs=ys=9;
	}

	while( ( x<0 || y<0 || map_getcell(m,x,y,CELL_CHKNOPASS)) && (i++)<1000 ){
		if( xs>0 && ys>0 && i<250 ){	// �w��ʒu�t�߂̒T��
			x=bx+rand()%xs-xs/2;
			y=by+rand()%ys-ys/2;
		}else{			// ���S�����_���T��
			x=rand()%(map[m].xs-2)+1;
			y=rand()%(map[m].ys-2)+1;
		}
	}
	md.dir=0;
	if(i<1000){
		md.bl.x=md.to_x=x;
		md.bl.y=md.to_y=y;
		md.bl.m=m;
	}else {
		m=md.bl.m;
		if(battle_config.error_log==1)
			ShowMessage("MOB %d warp failed, class_ = %d\n",md.bl.id,md.class_);
	}

	md.target_id=0;	// �^�Q����������
	md.state.targettype=NONE_ATTACKABLE;
	md.attacked_id=0;
	md.state.skillstate=MSS_IDLE;
	mob_changestate(md,MS_IDLE,0);

	if(type>0 && i==1000) {
		if(battle_config.battle_log)
			ShowMessage("MOB %d warp to (%d,%d), class_ = %d\n",md.bl.id,x,y,md.class_);
	}

	map_addblock(md.bl);
	skill_unit_move(md.bl,tick,1);
	if(type>0)
	{
		clif_spawnmob(md);
		mob_warpslave(md,md.bl.x,md.bl.y);
	}

	return 0;
}

/*==========================================
 * ��ʓ��̎�芪���̐��v�Z�p(foreachinarea)
 *------------------------------------------
 */
int mob_countslave_sub(struct block_list &bl,va_list ap)
{
	unsigned long id;
	int *c;
	struct mob_data &md = (struct mob_data &)bl;

	id=va_arg(ap,unsigned long);

	nullpo_retr(0, ap);
	nullpo_retr(0, c=va_arg(ap,int *));

	if( md.master_id==id )
		(*c)++;
	return 0;
}
/*==========================================
 * ��ʓ��̎�芪���̐��v�Z
 *------------------------------------------
 */
unsigned int mob_countslave(struct mob_data &md)
{
	unsigned int c=0;
	map_foreachinarea(mob_countslave_sub, md.bl.m,
		0,0,map[md.bl.m].xs-1,map[md.bl.m].ys-1,
		BL_MOB,md.bl.id,&c);
	return c;
}
/*==========================================
 * �艺MOB����
 *------------------------------------------
 */
int mob_summonslave(struct mob_data &md2,int *value,int amount,int flag)
{
	struct mob_data *md;
	int bx,by,m, count = 0, class_, k,i;

	while(count < 5 && value[count] > 1000 && value[count] <= MAX_MOB_DB) count++;
	if(count < 1) return 0;// �l���ُ�Ȃ珢�����~�߂�

	bx = md2.bl.x;
	by = md2.bl.y;
	m  = md2.bl.m;

	for(k=0;k<count;k++)
	{
		class_ = value[k];

		for(i=0;i<amount;i++)
		{
			int x=0,y=0,t=0;
			md=(struct mob_data *)aCalloc(1,sizeof(struct mob_data));
			if(mob_db[class_].mode&0x02)
				md->lootitem=(struct item *)aCalloc(LOOTITEM_SIZE,sizeof(struct item));
			else
				md->lootitem=NULL;

			do{
				x=bx + rand()%9-4;
				y=by + rand()%9-4;
			}while( map_getcell(m,x,y,CELL_CHKNOPASS) && ((t++)<100));
			if(t>=100){
				x=bx;
				y=by;
			}

			mob_spawn_dataset(*md,"--ja--",class_);
			md->bl.m=m;
			md->bl.x=x;
			md->bl.y=y;
			md->speed=md2.speed;

			memset(md->npc_event,0,sizeof(md->npc_event));
			md->bl.type=BL_MOB;
			map_addiddb(md->bl);
			mob_spawn(md->bl.id);
			clif_skill_nodamage(md->bl,md->bl,(flag)? NPC_SUMMONSLAVE:NPC_SUMMONMONSTER,amount,1);

			if(flag)
			{
				md->master_id=md2.bl.id;
				md2.state.is_master = true;
			}
		}
	}
	return 0;
}

/*==========================================
 *MOBskill����Y��skillid��skillidx��Ԃ�
 *------------------------------------------
 */
int mob_skillid2skillidx(int class_,unsigned short skillid)
{
	int i;
	struct mob_skill *ms=mob_db[class_].skill;

	if(ms==NULL)
		return -1;

	for(i=0;i<mob_db[class_].maxskill;i++){
		if(ms[i].skill_id == skillid)
			return i;
	}
	return -1;
}

//
// MOB�X�L��
//

/*==========================================
 * �X�L���g�p�i�r�������AID�w��j
 *------------------------------------------
 */
int mobskill_castend_id( int tid, unsigned long tick, int id,int data )
{
	struct mob_data* md=NULL;
	struct block_list *bl;
	struct block_list *mbl;
	int range;

	if((mbl = map_id2bl(id)) == NULL ) //�r������Mob���������Ȃ��Ƃ����̂͗ǂ����鐳�폈��
		return 0;
	if((md=(struct mob_data *)mbl) == NULL ){
		ShowMessage("mobskill_castend_id nullpo mbl->id:%d\n",mbl->id);
		return 0;
	}

	if( md->bl.type!=BL_MOB || md->bl.prev==NULL )
		return 0;

	if( md->skilltimer != tid )	// �^�C�}ID�̊m�F
		return 0;

	md->skilltimer=-1;
	//���ق��Ԉُ�Ȃ�
	if(md->sc_data){
		if(md->opt1>0 || md->sc_data[SC_DIVINA].timer != -1 ||
			(!(mob_db[md->class_].mode & 0x20) && md->sc_data[SC_ROKISWEIL].timer != -1) ||
			md->sc_data[SC_STEELBODY].timer != -1)
			return 0;
		if(md->sc_data[SC_AUTOCOUNTER].timer != -1 && md->skillid != KN_AUTOCOUNTER) //�I�[�g�J�E���^�[
			return 0;
		if(md->sc_data[SC_BLADESTOP].timer != -1) //���n���
			return 0;
		if(md->sc_data[SC_BERSERK].timer != -1) //�o�[�T�[�N
			return 0;
	}
	if(md->skillid != NPC_EMOTION)
		md->last_thinktime=tick + status_get_adelay(&md->bl);

	if((bl = map_id2bl(md->skilltarget)) == NULL || bl->prev==NULL){ //�X�L���^�[�Q�b�g�����݂��Ȃ�
		//ShowMessage("mobskill_castend_id nullpo\n");//�^�[�Q�b�g�����Ȃ��Ƃ���nullpo����Ȃ��ĕ��ʂɏI��
		return 0;
	}
	if(md->bl.m != bl->m)
		return 0;

	if(md->skillid == PR_LEXAETERNA) {
		struct status_change *sc_data = status_get_sc_data(bl);
		if(sc_data && (sc_data[SC_FREEZE].timer != -1 || (sc_data[SC_STONE].timer != -1 && sc_data[SC_STONE].val2 == 0)))
			return 0;
	}
	else if(md->skillid == RG_BACKSTAP) {
		int dir = map_calc_dir(md->bl,bl->x,bl->y),t_dir = status_get_dir(bl);
		int dist = distance(md->bl.x,md->bl.y,bl->x,bl->y);
		if(bl->type != BL_SKILL && (dist == 0 || map_check_dir(dir,t_dir)))
			return 0;
	}
	if( ( (skill_get_inf(md->skillid)&1) || (skill_get_inf2(md->skillid)&4) ) &&	// �މ�G�Ί֌W�`�F�b�N
		battle_check_target(&md->bl,bl, BCT_ENEMY)<=0 )
		return 0;
	range = skill_get_range(md->skillid,md->skilllv);
	if(range < 0)
		range = status_get_range(&md->bl) - (range + 1);
	if(range + (int)battle_config.mob_skill_add_range < distance(md->bl.x,md->bl.y,bl->x,bl->y))
		return 0;

	md->skilldelay[md->skillidx]=tick;

	if(battle_config.mob_skill_log)
		ShowMessage("MOB skill castend skill=%d, class_ = %d\n",md->skillid,md->class_);
	mob_stop_walking(*md,0);

	switch( skill_get_nk(md->skillid) )
	{
	case NK_NO_DAMAGE:// �x���n
		if(!mob_db[md->class_].skill[md->skillidx].val[0] &&
			(md->skillid==AL_HEAL || (md->skillid==ALL_RESURRECTION && bl->type != BL_PC)) && battle_check_undead(status_get_race(bl),status_get_elem_type(bl)) )
			skill_castend_damage_id(&md->bl,bl,md->skillid,md->skilllv,tick,0);
		else
			skill_castend_nodamage_id(&md->bl,bl,md->skillid,md->skilllv,tick,0);
		break;
	// �U���n/������΂��n
	case NK_SPLASH_DAMAGE:
	default:
		skill_castend_damage_id(&md->bl,bl,md->skillid,md->skilllv,tick,0);
		break;
	}


	return 0;
}

/*==========================================
 * �X�L���g�p�i�r�������A�ꏊ�w��j
 *------------------------------------------
 */
int mobskill_castend_pos( int tid, unsigned long tick, int id,int data )
{
	struct mob_data* md=NULL;
	int range,maxcount;

	nullpo_retr(0, md=(struct mob_data *)map_id2bl(id));

	if( md->bl.type!=BL_MOB || md->bl.prev==NULL )
		return 0;

	if( md->skilltimer != tid )	// �^�C�}ID�̊m�F
		return 0;

	md->skilltimer=-1;
	if(md->sc_data){
		if(md->opt1>0 || md->sc_data[SC_DIVINA].timer != -1 ||
			(!(mob_db[md->class_].mode & 0x20) && md->sc_data[SC_ROKISWEIL].timer != -1) ||
			md->sc_data[SC_STEELBODY].timer != -1)
			return 0;
		if(md->sc_data[SC_AUTOCOUNTER].timer != -1 && md->skillid != KN_AUTOCOUNTER) //�I�[�g�J�E���^�[
			return 0;
		if(md->sc_data[SC_BLADESTOP].timer != -1) //���n���
			return 0;
		if(md->sc_data[SC_BERSERK].timer != -1) //�o�[�T�[�N
			return 0;
	}

	if (!battle_config.monster_skill_reiteration &&
			skill_get_unit_flag (md->skillid) & UF_NOREITERATION &&
			skill_check_unit_range (md->bl.m, md->skillx, md->skilly, md->skillid, md->skilllv))
		return 0;

	if(battle_config.monster_skill_nofootset &&
			skill_get_unit_flag (md->skillid) & UF_NOFOOTSET &&
			skill_check_unit_range2(md->bl.m, md->skillx, md->skilly, md->skillid, md->skilllv, md->bl.type))
		return 0;


	if(battle_config.monster_land_skill_limit) {
		maxcount = skill_get_maxcount(md->skillid);
		if(maxcount > 0) {
			int i,c;
			for(i=c=0;i<MAX_MOBSKILLUNITGROUP;i++) {
				if(md->skillunit[i].alive_count > 0 && md->skillunit[i].skill_id == md->skillid)
					c++;
			}
			if(c >= maxcount)
				return 0;
		}
	}

	range = skill_get_range(md->skillid,md->skilllv);
	if(range < 0)
		range = status_get_range(&md->bl) - (range + 1);
	if(range + (int)battle_config.mob_skill_add_range < distance(md->bl.x,md->bl.y,md->skillx,md->skilly))
		return 0;
	md->skilldelay[md->skillidx]=tick;

	if(battle_config.mob_skill_log)
		ShowMessage("MOB skill castend skill=%d, class_ = %d\n",md->skillid,md->class_);
	mob_stop_walking(*md,0);

	skill_castend_pos2(&md->bl,md->skillx,md->skilly,md->skillid,md->skilllv,tick,0);

	return 0;
}


/*==========================================
 * Skill use (an aria start, ID specification)
 *------------------------------------------
 */
int mobskill_use_id(struct mob_data &md,struct block_list *target,unsigned short skill_idx)
{
	int casttime,range;
	struct mob_skill *ms;
	short skill_id, skill_lv;
	int forcecast = 0;
	int selfdestruct_flag = 0;

	nullpo_retr(0, ms=&mob_db[md.class_].skill[skill_idx]);

	if( target==NULL && (target=map_id2bl(md.target_id))==NULL )
		return 0;

	if( target->prev==NULL || md.bl.prev==NULL )
		return 0;

	if(md.skilltimer != -1)	// already casting
		return 0;

	skill_id=ms->skill_id;
	skill_lv=ms->skill_lv;

	// ���ق�ُ�
	if(md.sc_data){
		if(md.opt1>0 || md.sc_data[SC_DIVINA].timer != -1 ||
			(!(mob_db[md.class_].mode & 0x20) && md.sc_data[SC_ROKISWEIL].timer != -1) ||
			md.sc_data[SC_STEELBODY].timer != -1)
			return 0;
		if(md.sc_data[SC_AUTOCOUNTER].timer != -1 && md.skillid != KN_AUTOCOUNTER) //�I�[�g�J�E���^�[
			return 0;
		if(md.sc_data[SC_BLADESTOP].timer != -1) //���n���
			return 0;
		if(md.sc_data[SC_BERSERK].timer != -1) //�o�[�T�[�N
			return 0;
	}

	if(md.option&4 && skill_id == TF_HIDING)
		return 0;
	if(md.option&2 && skill_id != TF_HIDING && skill_id != AS_GRIMTOOTH &&
		skill_id != RG_BACKSTAP && skill_id != RG_RAID &&
		skill_id != AM_POTIONPITCHER && skill_id != AL_HEAL)
		return 0;

	if(map[md.bl.m].flag.gvg && skill_db[skill_id].nocast & 4)
		return 0;
	if(skill_get_inf2(skill_id)&INF2_NO_TARGET_SELF && md.bl.id == target->id)
		return 0;

	// �˒��Ə�Q���`�F�b�N
	range = skill_get_range(skill_id,skill_lv);
	if(range < 0)
		range = status_get_range(&md.bl) - (range + 1);
	if(!battle_check_range(&md.bl,target,range))
		return 0;

	casttime=skill_castfix(&md.bl, ms->casttime);
	md.state.skillcastcancel=ms->cancel;
	md.skilldelay[skill_idx]=gettick();

	switch(skill_id){	/* ��������ȏ������K�v */
	case ALL_RESURRECTION:	/* ���U���N�V���� */
		if(target->type != BL_PC && battle_check_undead(status_get_race(target),status_get_elem_type(target))){	/* �G���A���f�b�h�Ȃ� */
			forcecast=1;	/* �^�[���A���f�b�g�Ɠ����r������ */
			casttime=skill_castfix(&md.bl, skill_get_cast(PR_TURNUNDEAD,skill_lv) );
		}
		break;
	case MO_EXTREMITYFIST:	/*���C���e�P��*/
	case SA_MAGICROD:
	case SA_SPELLBREAKER:
		forcecast=1;
		break;
	case NPC_SUMMONSLAVE:
	case NPC_SUMMONMONSTER:
		if(md.master_id!=0)
			return 0;
		break;
	case NPC_SELFDESTRUCTION:
		if (casttime == 0 && md.state.special_mob_ai == 2) {
			casttime = skill_get_time(skill_id,skill_lv);
			selfdestruct_flag =  1;
		}
		break;
	}

	if(battle_config.mob_skill_log)
		ShowMessage("MOB skill use target_id=%d skill=%d lv=%d cast=%d, class_ = %d\n",target->id,skill_id,skill_lv,casttime,md.class_);

	if(casttime || forcecast)
	{	// �r�����K�v

		if (!selfdestruct_flag)
			mob_stop_walking(md,0);		// ���s��~
		clif_skillcasting(md.bl, md.bl.id, target->id, 0,0, skill_id, casttime);
	}

	if (casttime <= 0)	// �r���̖������̂̓L�����Z������Ȃ�
		md.state.skillcastcancel=0;

	md.skilltarget	= target->id;
	md.skillx		= 0;
	md.skilly		= 0;
	md.skillid		= skill_id;
	md.skilllv		= skill_lv;
	md.skillidx		= skill_idx;

	if(!(battle_config.monster_cloak_check_type&2) && md.sc_data[SC_CLOAKING].timer != -1 && md.skillid != AS_CLOAKING)
		status_change_end(&md.bl,SC_CLOAKING,-1);

	if( casttime>0 ){
		md.skilltimer =
			add_timer( gettick()+casttime, mobskill_castend_id, md.bl.id, 0 );
	}else{
		md.skilltimer = -1;
		mobskill_castend_id(md.skilltimer,gettick(),md.bl.id, 0);
	}

	return 1;
}
/*==========================================
 * �X�L���g�p�i�ꏊ�w��j
 *------------------------------------------
 */
int mobskill_use_pos( struct mob_data *md, int skill_x, int skill_y, unsigned short skill_idx)
{
	int casttime=0,range;
	struct mob_skill *ms;
	struct block_list bl;
	unsigned short skill_id, skill_lv;

	nullpo_retr(0, md);
	nullpo_retr(0, ms=&mob_db[md->class_].skill[skill_idx]);

	if( md->bl.prev==NULL )
		return 0;
	if(md->skilltimer != -1)	// already casting
		return 0;

	skill_id=ms->skill_id;
	skill_lv=ms->skill_lv;

	//���ق��Ԉُ�Ȃ�
	if(md->sc_data){
		if(md->opt1>0 || md->sc_data[SC_DIVINA].timer != -1 ||
			(!(mob_db[md->class_].mode & 0x20) && md->sc_data[SC_ROKISWEIL].timer != -1) ||
			md->sc_data[SC_STEELBODY].timer != -1)
			return 0;
		if(md->sc_data[SC_AUTOCOUNTER].timer != -1 && md->skillid != KN_AUTOCOUNTER) //�I�[�g�J�E���^�[
			return 0;
		if(md->sc_data[SC_BLADESTOP].timer != -1) //���n���
			return 0;
		if(md->sc_data[SC_BERSERK].timer != -1) //�o�[�T�[�N
			return 0;
	}

	if(md->option&2)
		return 0;

	if(map[md->bl.m].flag.gvg && (skill_id == SM_ENDURE || skill_id == AL_TELEPORT || skill_id == AL_WARP ||
		skill_id == WZ_ICEWALL || skill_id == TF_BACKSLIDING))
		return 0;

	// �˒��Ə�Q���`�F�b�N
	bl.type = BL_NUL;
	bl.m = md->bl.m;
	bl.x = skill_x;
	bl.y = skill_y;
	range = skill_get_range(skill_id,skill_lv);
	if(range < 0)
		range = status_get_range(&md->bl) - (range + 1);
	if(!battle_check_range(&md->bl,&bl,range))
		return 0;

	casttime=skill_castfix(&md->bl,ms->casttime);
	md->state.skillcastcancel=ms->cancel;
	md->skilldelay[skill_idx]=gettick();

	if(battle_config.mob_skill_log)
		ShowMessage("MOB skill use target_pos=(%d,%d) skill=%d lv=%d cast=%d, class_ = %d\n",
			skill_x,skill_y,skill_id,skill_lv,casttime,md->class_);

	if( casttime>0 ) {	// A cast time is required.
		mob_stop_walking(*md,0);		// ���s��~
		clif_skillcasting(md->bl,
			md->bl.id, 0, skill_x,skill_y, skill_id,casttime);
	}

	if( casttime<=0 )	// A skill without a cast time wont be cancelled.
		md->state.skillcastcancel=0;

	md->skillx		= skill_x;
	md->skilly		= skill_y;
	md->skilltarget	= 0;
	md->skillid		= skill_id;
	md->skilllv		= skill_lv;
	md->skillidx	= skill_idx;

	if(!(battle_config.monster_cloak_check_type&2) && md->sc_data[SC_CLOAKING].timer != -1)
		status_change_end(&md->bl,SC_CLOAKING,-1);

	if( casttime>0 ){
		md->skilltimer =
			add_timer( gettick()+casttime, mobskill_castend_pos, md->bl.id, 0 );
	}else{
		md->skilltimer = -1;
		mobskill_castend_pos(md->skilltimer,gettick(),md->bl.id, 0);
	}

	return 1;
}


/*==========================================
 * Friendly Mob whose HP is decreasing by a nearby MOB is looked for.
 *------------------------------------------
 */
int mob_getfriendhpltmaxrate_sub(struct block_list &bl,va_list ap)
{
	int rate;
	struct mob_data **fr, *mmd;
	struct mob_data &md=(struct mob_data &)bl;

	nullpo_retr(0, ap);
	nullpo_retr(0, mmd=va_arg(ap,struct mob_data *));

	if( mmd->bl.id == bl.id )
		return 0;
	if (battle_check_target(&mmd->bl,&bl,BCT_ENEMY)>0)
		return 0;
	rate=va_arg(ap,int);
	fr=va_arg(ap,struct mob_data **);
	if (md.hp < md.max_hp * rate / 100)
		(*fr) = &md;
	return 0;
}
struct mob_data *mob_getfriendhpltmaxrate(struct mob_data &md,int rate)
{
	struct mob_data *fr=NULL;
	const int r=8;

	map_foreachinarea(mob_getfriendhpltmaxrate_sub, md.bl.m,
		((int)md.bl.x)-r ,((int)md.bl.y)-r, ((int)md.bl.x)+r, ((int)md.bl.y)+r,
		BL_MOB, &md, rate, &fr);
	return fr;
}
/*==========================================
 * Check hp rate of its master
 *------------------------------------------
 */
struct block_list *mob_getmasterhpltmaxrate(struct mob_data &md,int rate)
{
	if (md.master_id > 0) {
		struct block_list *bl = map_id2bl(md.master_id);
		if (status_get_hp(bl) < status_get_max_hp(bl) * rate / 100)
			return bl;
	}

	return NULL;
}
/*==========================================
 * What a status state suits by nearby MOB is looked for.
 *------------------------------------------
 */
int mob_getfriendstatus_sub(struct block_list &bl,va_list ap)
{
	int cond1,cond2;
	struct mob_data **fr, *mmd;
	struct mob_data &md = (struct mob_data &)bl;
	int flag=0;

	nullpo_retr(0, ap);
	nullpo_retr(0, mmd=va_arg(ap,struct mob_data *));

	if( mmd->bl.id == bl.id )
		return 0;
	if (battle_check_target(&mmd->bl,&bl,BCT_ENEMY)>0)
		return 0;
	cond1=va_arg(ap,int);
	cond2=va_arg(ap,int);
	fr=va_arg(ap,struct mob_data **);
	if( cond2==-1 ){
		int j;
		for(j=SC_STONE;j<=SC_BLIND && !flag;j++){
			flag=(md.sc_data[j].timer!=-1 );
		}
	}else
		flag=( md.sc_data[cond2].timer!=-1 );
	if( flag^( cond1==MSC_FRIENDSTATUSOFF ) )
		(*fr)=&md;

	return 0;
}
struct mob_data *mob_getfriendstatus(struct mob_data &md,int cond1,int cond2)
{
	struct mob_data *fr=NULL;
	const int r=8;

	map_foreachinarea(mob_getfriendstatus_sub, md.bl.m,
		((int)md.bl.x)-r ,((int)md.bl.y)-r, ((int)md.bl.x)+r, ((int)md.bl.y)+r,
		BL_MOB,&md,cond1,cond2,&fr);
	return fr;
}

/*==========================================
 * Skill use judging
 *------------------------------------------
 */
int mobskill_use(struct mob_data &md,unsigned long tick,int event)
{
	struct mob_skill *ms;
	struct mob_data *fmd = NULL;
	struct map_session_data *fsd = NULL;
	int i;

	nullpo_retr (0, ms = mob_db[md.class_].skill);

	if (battle_config.mob_skill_rate == 0 || md.skilltimer != -1 )
		return 0;

	for (i = 0; i < mob_db[md.class_].maxskill; i++) {
		int c2 = ms[i].cond2, flag = 0;

		// �f�B���C��
		if (DIFF_TICK(tick, md.skilldelay[i]) < ms[i].delay)
			continue;

		// ��Ԕ���
		if( ms[i].state>=0 && ms[i].state!=(short)(md.state.skillstate) )
			continue;

		// ��������
		flag = (event == ms[i].cond1);
		if (!flag){
			switch (ms[i].cond1)
			{
				case MSC_ALWAYS:
					flag = 1;
					break;
				case MSC_MYHPLTMAXRATE:		// HP< maxhp%
				{
					long max_hp = status_get_max_hp(&md.bl);
					flag = (md.hp < max_hp * c2 / 100);
					break;
				}
				case MSC_MYSTATUSON:		// status[num] on
				case MSC_MYSTATUSOFF:		// status[num] off
					if (!md.sc_data) {
						flag = 0;
					} else if (ms[i].cond2 == -1) {
						int j;
						for (j = SC_STONE; j <= SC_BLIND; j++)
							if ((flag = (md.sc_data[j].timer != -1)) != 0)
								break;
					} else {
						flag = (md.sc_data[ms[i].cond2].timer != -1);
					}
					flag ^= (ms[i].cond1 == MSC_MYSTATUSOFF);
					break;
				case MSC_FRIENDHPLTMAXRATE:	// friend HP < maxhp%
					flag = ((fmd = mob_getfriendhpltmaxrate(md, ms[i].cond2)) != NULL);
					break;
				case MSC_FRIENDSTATUSON:	// friend status[num] on
				case MSC_FRIENDSTATUSOFF:	// friend status[num] off
					flag = ((fmd = mob_getfriendstatus(md, ms[i].cond1, ms[i].cond2)) != NULL);
					break;
				case MSC_SLAVELT:		// slave < num
					flag = (mob_countslave(md) < (unsigned int)c2 );
					break;
				case MSC_ATTACKPCGT:	// attack pc > num
					flag = (battle_counttargeted(md.bl, NULL, 0) > (unsigned int)c2);
					break;
				case MSC_SLAVELE:		// slave <= num
					flag = (mob_countslave(md) <= (unsigned int)c2 );
					break;
				case MSC_ATTACKPCGE:	// attack pc >= num
					flag = (battle_counttargeted(md.bl, NULL, 0) >= (unsigned int)c2);
					break;
				case MSC_SKILLUSED:		// specificated skill used
					flag = ((event & 0xffff) == MSC_SKILLUSED && ((event >> 16) == c2 || c2 == 0));
					break;
				case MSC_RUDEATTACKED:
					flag = (!md.attacked_id && md.attacked_count > 0);
					if (flag) md.attacked_count = 0;	//Rude attacked count should be reset after the skill condition is met. Thanks to Komurka [Skotlex]
					break;
				case MSC_MASTERHPLTMAXRATE:
				{
						struct block_list *bl = mob_getmasterhpltmaxrate(md, ms[i].cond2);
						if (bl) {
							if (bl->type == BL_MOB)
								fmd=(struct mob_data *)bl;
							else if (bl->type == BL_PC)
								fsd=(struct map_session_data *)bl;
						}
					flag = (fmd || fsd);
					break;
				}
				case MSC_MASTERATTACKED:
				{
					block_list * bl = map_id2bl(md.master_id);

					flag = (md.master_id>0 && bl && battle_counttargeted(*bl, NULL, 0) > 0);
					break;

				}
				case MSC_ALCHEMIST:
					flag = (md.state.alchemist);
					break;
			}
		}

		// �m������
		if (flag && rand() % 10000 < ms[i].permillage)
		{
			if (skill_get_inf(ms[i].skill_id) & 2) {
				// �ꏊ�w��
				struct block_list *bl = NULL;
				int x = 0, y = 0;
				if (ms[i].target <= MST_AROUND) {
					switch (ms[i].target) {
						case MST_TARGET:
						case MST_AROUND5:
							bl = map_id2bl(md.target_id);
							break;
						case MST_FRIEND:
							if (fmd) {
								bl = &fmd->bl;
								break;
							} else if (fsd) {
								bl = &fsd->bl;
								break;
							} // else fall through
						default:
							bl = &md.bl;
							break;
					}
					if (bl != NULL) {
						x = bl->x; y=bl->y;
					}
				}
				if (x <= 0 || y <= 0)
					continue;
				// �����̎���
				if (ms[i].target >= MST_AROUND1) {
					int bx = x, by = y, i = 0, m = bl->m, r = ms[i].target-MST_AROUND1;
					do {
						bx = x + rand() % (r*2+3) - r;
						by = y + rand() % (r*2+3) - r;
					} while ((
						//bx <= 0 || by <= 0 || bx >= map[m].xs || by >= map[m].ys ||	// checked in getcell
						map_getcell(m, bx, by, CELL_CHKNOPASS)) && (i++) < 1000);
					if (i < 1000){
						x = bx; y = by;
					}
				}
				// ����̎���
				if (ms[i].target >= MST_AROUND5) {
					int bx = x, by = y, i = 0, m = bl->m, r = (ms[i].target-MST_AROUND5) + 1;
					do {
						bx = x + rand() % (r*2+1) - r;
						by = y + rand() % (r*2+1) - r;
					} while ((
						//bx <= 0 || by <= 0 || bx >= map[m].xs || by >= map[m].ys ||	// checked in getcell
						map_getcell(m, bx, by, CELL_CHKNOPASS)) && (i++) < 1000);
					if (i < 1000){
						x = bx; y = by;
					}
				}
				if (!mobskill_use_pos(&md, x, y, i))
					return 0;
			} else {
				// ID�w��
				if (ms[i].target <= MST_FRIEND) {
					struct block_list *bl;
					switch (ms[i].target) {
						case MST_TARGET:
							bl = map_id2bl(md.target_id);
							break;
						case MST_FRIEND:
							if (fmd) {
								bl = &fmd->bl;
								break;
							} else if (fsd) {
								bl = &fsd->bl;
								break;
							} // else fall through
						default:
							bl = &md.bl;
							break;
					}
					if (bl && !mobskill_use_id(md, bl, i))
						return 0;
				}
			}
			if (ms[i].emotion >= 0)
				clif_emotion(md.bl, ms[i].emotion);
			return 1;
		}
	}

	return 0;
}
/*==========================================
 * Skill use event processing
 *------------------------------------------
 */
int mobskill_event(struct mob_data &md, int flag)
{
	int tick = gettick();
	if (flag == -1 && mobskill_use(md, tick, MSC_CASTTARGETED))
		return 1;
	if ((flag & BF_SHORT) && mobskill_use(md, tick, MSC_CLOSEDATTACKED))
		return 1;
	if ((flag & BF_LONG) && mobskill_use(md, tick, MSC_LONGRANGEATTACKED))
		return 1;
	return 0;
}
/*==========================================
 * Mob���G���y���E���Ȃǂ̏ꍇ�̔���
 *------------------------------------------
 */
int mob_gvmobcheck(struct map_session_data &sd, struct block_list *bl)
{
	struct mob_data *md=NULL;

	nullpo_retr(0,bl);

	if(bl->type==BL_MOB && (md=(struct mob_data *)bl) &&
		(md->class_ == 1288 || md->class_ == 1287 || md->class_ == 1286 || md->class_ == 1285))
	{
		struct guild_castle *gc=guild_mapname2gc(map[sd.bl.m].mapname);
		struct guild *g=guild_search(sd.status.guild_id);

		if(g == NULL && md->class_ == 1288)
			return 0;//�M���h�������Ȃ�_���[�W����
		else if(gc != NULL && !map[sd.bl.m].flag.gvg)
			return 0;//�ԓ���Gv����Ȃ��Ƃ��̓_���[�W�Ȃ�
		else if(g) {
			if (gc != NULL && g->guild_id == gc->guild_id)
				return 0;//����̃M���h�̃G���y�Ȃ�_���[�W����
			else if(guild_checkskill(*g,GD_APPROVAL) <= 0 && md->class_ == 1288)
				return 0;//���K�M���h���F���Ȃ��ƃ_���[�W����
			else if (gc && guild_check_alliance(gc->guild_id, g->guild_id, 0) == 1)
				return 0;	// �����Ȃ�_���[�W����
		}
	}
	return 1;
}
/*==========================================
 * �X�L���p�^�C�}�[�폜
 *------------------------------------------
 */
int mobskill_deltimer(struct mob_data &md )
{
	if( md.skilltimer!=-1 ){
		if( skill_get_inf( md.skillid )& INF_GROUND_SKILL )
			delete_timer( md.skilltimer, mobskill_castend_pos );
		else
			delete_timer( md.skilltimer, mobskill_castend_id );
		md.skilltimer=-1;
	}
	return 0;
}
//
// ������
//
/*==========================================
 * Since un-setting [ mob ] up was used, it is an initial provisional value setup.
 *------------------------------------------
 */
int mob_makedummymobdb(int class_)
{
	int i;

	sprintf(mob_db[class_].name,"mob%d",class_);
	sprintf(mob_db[class_].jname,"mob%d",class_);
	mob_db[class_].lv=1;
	mob_db[class_].max_hp=1000;
	mob_db[class_].max_sp=1;
	mob_db[class_].base_exp=2;
	mob_db[class_].job_exp=1;
	mob_db[class_].range=1;
	mob_db[class_].atk1=7;
	mob_db[class_].atk2=10;
	mob_db[class_].def=0;
	mob_db[class_].mdef=0;
	mob_db[class_].str=1;
	mob_db[class_].agi=1;
	mob_db[class_].vit=1;
	mob_db[class_].int_=1;
	mob_db[class_].dex=6;
	mob_db[class_].luk=2;
	mob_db[class_].range2=10;
	mob_db[class_].range3=10;
	mob_db[class_].size=0;
	mob_db[class_].race=0;
	mob_db[class_].element=0;
	mob_db[class_].mode=0;
	mob_db[class_].speed=300;
	mob_db[class_].adelay=1000;
	mob_db[class_].amotion=500;
	mob_db[class_].dmotion=500;
	//mob_db[class_].dropitem[0].nameid=909;	// Jellopy
	//mob_db[class_].dropitem[0].p=1000;
	for(i=1;i<10;i++){ // 8-> 10 Lupus
		mob_db[class_].dropitem[i].nameid=0;
		mob_db[class_].dropitem[i].p=0;
	}
	// Item1,Item2
	mob_db[class_].mexp=0;
	mob_db[class_].mexpper=0;
	for(i=0;i<3;i++){
		mob_db[class_].mvpitem[i].nameid=0;
		mob_db[class_].mvpitem[i].p=0;
	}
	for(i=0;i<MAX_RANDOMMONSTER;i++)
		mob_db[class_].summonper[i]=0;
	return 0;
}

/*==========================================
 * db/mob_db.txt reading
 *------------------------------------------
 */
int mob_readdb(void)
{
	FILE *fp;
	char line[1024];
	char *filename[]={ "db/mob_db.txt","db/mob_db2.txt" };
	int i;

	memset(mob_db,0,sizeof(mob_db));

	for(i=0;i<2;i++){

		fp=safefopen(filename[i],"r");
		if(fp==NULL){
			if(i>0)
				continue;
			return -1;
		}
		while(fgets(line,1020,fp)){
			int class_, i;
			long exp, maxhp;
			char *str[60], *p, *np; // 55->60 Lupus

			if( !skip_empty_line(line) )
				continue;

			for(i=0,p=line;i<60;i++){
				if((np=strchr(p,','))!=NULL){
					str[i]=p;
					*np=0;
					p=np+1;
				} else
					str[i]=p;
			}

			class_ = atoi(str[0]);
			if (class_ <= 1000 || class_ > MAX_MOB_DB)
				continue;

			mob_db[class_].view_class = class_;
			memcpy(mob_db[class_].name, str[1], 24);
			memcpy(mob_db[class_].jname, str[2], 24);
			mob_db[class_].lv = atoi(str[3]);
			mob_db[class_].max_hp = atoi(str[4]);
			mob_db[class_].max_sp = atoi(str[5]);

			exp = (atoi(str[6]) * battle_config.base_exp_rate / 100);
			if (exp < 0)
				exp = 0;
			else if (exp > 0x7fffffff) exp = 0x7fffffff;
			mob_db[class_].base_exp = (int)exp;

			exp = (atoi(str[7]) * battle_config.job_exp_rate / 100);
			if (exp < 0)
				exp = 0;
			else if (exp > 1000000000)
				exp = 1000000000;

			mob_db[class_].job_exp = exp;

			mob_db[class_].range=atoi(str[8]);
			mob_db[class_].atk1=atoi(str[9]);
			mob_db[class_].atk2=atoi(str[10]);
			mob_db[class_].def=atoi(str[11]);
			mob_db[class_].mdef=atoi(str[12]);
			mob_db[class_].str=atoi(str[13]);
			mob_db[class_].agi=atoi(str[14]);
			mob_db[class_].vit=atoi(str[15]);
			mob_db[class_].int_=atoi(str[16]);
			mob_db[class_].dex=atoi(str[17]);
			mob_db[class_].luk=atoi(str[18]);
			mob_db[class_].range2=atoi(str[19]);
			mob_db[class_].range3=atoi(str[20]);
			mob_db[class_].size=atoi(str[21]);
			mob_db[class_].race=atoi(str[22]);
			mob_db[class_].element=atoi(str[23]);
			mob_db[class_].mode=atoi(str[24]);
			mob_db[class_].speed=atoi(str[25]);
			mob_db[class_].adelay=atoi(str[26]);
			mob_db[class_].amotion=atoi(str[27]);
			mob_db[class_].dmotion=atoi(str[28]);

			for(i=0;i<10;i++){ // 8 -> 10 Lupus
				int rate = 0,type,ratemin,ratemax;
				mob_db[class_].dropitem[i].nameid=atoi(str[29+i*2]);
				type = itemdb_type(mob_db[class_].dropitem[i].nameid);
				if (type == 0) {
					rate = battle_config.item_rate_heal * atoi(str[30+i*2]) / 100; //fix by Yor
					ratemin = battle_config.item_drop_heal_min;
					ratemax = battle_config.item_drop_heal_max;
				}
				else if (type == 2) {
					rate = battle_config.item_rate_use * atoi(str[30+i*2]) / 100; //fix by Yor
					ratemin = battle_config.item_drop_use_min;
					ratemax = battle_config.item_drop_use_max;	// End
				}
				else if (type == 4 || type == 5 || type == 8) {		// Changed to include Pet Equip
					rate = battle_config.item_rate_equip * atoi(str[30+i*2]) / 100;
					ratemin = battle_config.item_drop_equip_min;
					ratemax = battle_config.item_drop_equip_max;
				}
				else if (type == 6) {
					rate = battle_config.item_rate_card * atoi(str[30+i*2]) / 100;
					ratemin = battle_config.item_drop_card_min;
					ratemax = battle_config.item_drop_card_max;
				}
				else {
					rate = battle_config.item_rate_common * atoi(str[30+i*2]) / 100;
					ratemin = battle_config.item_drop_common_min;
					ratemax = battle_config.item_drop_common_max;
				}
				mob_db[class_].dropitem[i].p = (rate < ratemin) ? ratemin : (rate > ratemax) ? ratemax: rate;
			}
			// MVP EXP Bonus, Chance: MEXP,ExpPer
			mob_db[class_].mexp=atoi(str[49])*battle_config.mvp_exp_rate/100;
			mob_db[class_].mexpper=atoi(str[50]);
			//Now that we know if it is an mvp or not,
			//apply battle_config modifiers [Skotlex]
			maxhp = mob_db[class_].max_hp;
			if (mob_db[class_].mexp > 0)
			{	//Mvp
				if (battle_config.mvp_hp_rate != 100)
					maxhp = maxhp * battle_config.mvp_hp_rate /100;
			}
			else if (battle_config.monster_hp_rate != 100) //Normal mob
				maxhp = maxhp * battle_config.monster_hp_rate /100;
			if (maxhp < 0) maxhp = 1;
			else if (maxhp > 0x7fffffff) maxhp = 0x7fffffff;
			mob_db[class_].max_hp = maxhp;

			// MVP Drops: MVP1id,MVP1per,MVP2id,MVP2per,MVP3id,MVP3per
			for(i=0;i<3;i++){
				int rate=atoi(str[52+i*2])*battle_config.mvp_item_rate/100; //idea of the fix from Freya
				mob_db[class_].mvpitem[i].nameid=atoi(str[51+i*2]);
				mob_db[class_].mvpitem[i].p = (rate < (int)battle_config.item_drop_mvp_min)
					? battle_config.item_drop_mvp_min : (rate > (int)battle_config.item_drop_mvp_max)
					? battle_config.item_drop_mvp_max : rate;
			}
			for(i=0;i<MAX_RANDOMMONSTER;i++)
				mob_db[class_].summonper[i]=0;
			mob_db[class_].maxskill=0;

			mob_db[class_].sex=0;
			mob_db[class_].hair=0;
			mob_db[class_].hair_color=0;
			mob_db[class_].weapon=0;
			mob_db[class_].shield=0;
			mob_db[class_].head_top=0;
			mob_db[class_].head_mid=0;
			mob_db[class_].head_buttom=0;
			mob_db[class_].clothes_color=0; //Add for player monster dye - Valaris
		}
		fclose(fp);
		ShowStatus("Done reading '"CL_WHITE"%s"CL_RESET"'.\n",filename[i]);
	}
	return 0;
}

/*==========================================
 * MOB display graphic change data reading
 *------------------------------------------
 */
int mob_readdb_mobavail(void)
{
	FILE *fp;
	char line[1024];
	unsigned long ln=0;
	int class_,j,k;
	char *str[20],*p,*np;

	if( (fp=safefopen("db/mob_avail.txt","r"))==NULL ){
		ShowMessage("can't read %s\n", "db/mob_avail.txt");
		return -1;
	}

	while(fgets(line,1020,fp)){
		if( !skip_empty_line(line) )
			continue;
		memset(str,0,sizeof(str));

		for(j=0,p=line;j<12;j++){
			if((np=strchr(p,','))!=NULL){
				str[j]=p;
				*np=0;
				p=np+1;
			} else
				str[j]=p;
		}

		if(str[0]==NULL)
			continue;

		class_=atoi(str[0]);
		if(class_<=1000 || class_>MAX_MOB_DB)	// �l���ُ�Ȃ珈�����Ȃ��B
			continue;

		k=atoi(str[1]);
		if(k < 0)
			continue;
		if (j > 3 && k > 23 && k < 69)
			k += 3977;	// advanced job/baby class
		mob_db[class_].view_class=k;

		if((k < 24) || (k > 4000)) {
			mob_db[class_].sex=atoi(str[2]);
			mob_db[class_].hair=atoi(str[3]);
			mob_db[class_].hair_color=atoi(str[4]);
			mob_db[class_].weapon=atoi(str[5]);
			mob_db[class_].shield=atoi(str[6]);
			mob_db[class_].head_top=atoi(str[7]);
			mob_db[class_].head_mid=atoi(str[8]);
			mob_db[class_].head_buttom=atoi(str[9]);
			mob_db[class_].option=atoi(str[10])&~0x46;
			mob_db[class_].clothes_color=atoi(str[11]); // Monster player dye option - Valaris
		}
		else if(atoi(str[2]) > 0) mob_db[class_].equip=atoi(str[2]); // mob equipment [Valaris]

		ln++;
	}
	fclose(fp);
	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n",ln,"db/mob_avail.txt");
	return 0;
}

/*==========================================
 * Reading of random monster data
 *------------------------------------------
 */
int mob_read_randommonster(void)
{
	FILE *fp;
	char line[1024];
	char *str[10],*p;
	int i,j;

	char* mobfile[] = {
		"db/mob_branch.txt",
		"db/mob_poring.txt",
		"db/mob_boss.txt" };

	for(i=0;i<MAX_RANDOMMONSTER;i++){
		mob_db[0].summonper[i] = 1002;	// �ݒ肵�Y�ꂽ�ꍇ�̓|�������o��悤�ɂ��Ă���
		fp=safefopen(mobfile[i],"r");
		if(fp==NULL){
			ShowMessage("can't read %s\n",mobfile[i]);
			return -1;
		}
		while(fgets(line,1020,fp)){
			int class_,per;
			if( !skip_empty_line(line) )
				continue;
			memset(str,0,sizeof(str));
			for(j=0,p=line;j<3 && p;j++){
				str[j]=p;
				p=strchr(p,',');
				if(p) *p++=0;
			}

			if(str[0]==NULL || str[2]==NULL)
				continue;

			class_ = atoi(str[0]);
			per=atoi(str[2]);
			if((class_>1000 && class_<=MAX_MOB_DB) || class_==0)
				mob_db[class_].summonper[i]=per;
		}
		fclose(fp);
		ShowStatus("Done reading '"CL_WHITE"%s"CL_RESET"'.\n",mobfile[i]);
	}
	return 0;
}
/*==========================================
 * db/mob_skill_db.txt reading
 *------------------------------------------
 */
int mob_readskilldb(void)
{
	FILE *fp;
	char line[1024];
	int i;

	const struct {
		char str[32];
		int id;
	} cond1[] = {
		{	"always",			MSC_ALWAYS				},
		{	"myhpltmaxrate",	MSC_MYHPLTMAXRATE		},
		{	"friendhpltmaxrate",MSC_FRIENDHPLTMAXRATE	},
		{	"mystatuson",		MSC_MYSTATUSON			},
		{	"mystatusoff",		MSC_MYSTATUSOFF			},
		{	"friendstatuson",	MSC_FRIENDSTATUSON		},
		{	"friendstatusoff",	MSC_FRIENDSTATUSOFF		},
		{	"attackpcgt",		MSC_ATTACKPCGT			},
		{	"attackpcge",		MSC_ATTACKPCGE			},
		{	"slavelt",			MSC_SLAVELT				},
		{	"slavele",			MSC_SLAVELE				},
		{	"closedattacked",	MSC_CLOSEDATTACKED		},
		{	"longrangeattacked",MSC_LONGRANGEATTACKED	},
		{	"skillused",		MSC_SKILLUSED			},
		{	"casttargeted",		MSC_CASTTARGETED		},
		{	"rudeattacked",		MSC_RUDEATTACKED		},
		{	"masterhpltmaxrate",MSC_MASTERHPLTMAXRATE	},
		{	"masterattacked",	MSC_MASTERATTACKED		},
		{	"alchemist",		MSC_ALCHEMIST			},
	}, cond2[] ={
		{	"anybad",		-1				},
		{	"stone",		SC_STONE		},
		{	"freeze",		SC_FREEZE		},
		{	"stan",			SC_STAN			},
		{	"sleep",		SC_SLEEP		},
		{	"poison",		SC_POISON		},
		{	"curse",		SC_CURSE		},
		{	"silence",		SC_SILENCE		},
		{	"confusion",	SC_CONFUSION	},
		{	"blind",		SC_BLIND		},
		{	"hiding",		SC_HIDING		},
		{	"sight",		SC_SIGHT		},
	}, state[] = {
		{	"any",		-1			},
		{	"idle",		MSS_IDLE	},
		{	"walk",		MSS_WALK	},
		{	"attack",	MSS_ATTACK	},
		{	"dead",		MSS_DEAD	},
		{	"loot",		MSS_LOOT	},
		{	"chase",	MSS_CHASE	},
	}, target[] = {
		{	"target",	MST_TARGET	},
		{	"self",		MST_SELF	},
		{	"friend",	MST_FRIEND	},
		{	"around5",	MST_AROUND5	},
		{	"around6",	MST_AROUND6	},
		{	"around7",	MST_AROUND7	},
		{	"around8",	MST_AROUND8	},
		{	"around1",	MST_AROUND1	},
		{	"around2",	MST_AROUND2	},
		{	"around3",	MST_AROUND3	},
		{	"around4",	MST_AROUND4	},
		{	"around",	MST_AROUND	},
	};

	int x;
	char *filename[]={ "db/mob_skill_db.txt","db/mob_skill_db2.txt" };

	for(x=0;x<2;x++){

		fp=safefopen(filename[x],"r");
		if(fp==NULL){
			if(x==0)
				ShowMessage("can't read %s\n",filename[x]);
			continue;
		}
		while(fgets(line,1020,fp)){
			char *sp[20],*p;
			int mob_id;
			struct mob_skill *ms=NULL;
			size_t j=0;

			if( !skip_empty_line(line) )
				continue;

			memset(sp,0,sizeof(sp));
			for(i=0,p=line;i<18 && p;i++){
				sp[i]=p;
				if((p=strchr(p,','))!=NULL)
					*p++=0;
			}
			mob_id=atoi(sp[0]);
			if( mob_id<=0  || mob_id>=MAX_MOB_DB)
				continue;

			if( strcmp(sp[1],"clear")==0 ){
				memset(mob_db[mob_id].skill,0,sizeof(mob_db[mob_id].skill));
				mob_db[mob_id].maxskill=0;
				continue;
			}

			for(i=0;i<MAX_MOBSKILL;i++)
				if( (ms=&mob_db[mob_id].skill[i])->skill_id == 0)
					break;
			if(i==MAX_MOBSKILL){
				ShowMessage("mob_skill: readdb: too many skill ! [%s] in %d[%s]\n",
					sp[1],mob_id,mob_db[mob_id].jname);
				continue;
			}

			ms->state=atoi(sp[2]);
			for(j=0;j<sizeof(state)/sizeof(state[0]);j++){
				if( strcmp(sp[2],state[j].str)==0)
					ms->state=state[j].id;
			}
			ms->skill_id=atoi(sp[3]);
			j=atoi(sp[4]);
			if (j<=0 || j>MAX_SKILL_DB)
				continue;
			ms->skill_lv=j;
			//Apply battle_config modifiers to rate (permillage) and delay [Skotlex]
			ms->permillage=atoi(sp[5]);
			if (battle_config.mob_skill_rate != 100)
				ms->permillage = ms->permillage*battle_config.mob_skill_rate/100;
			ms->casttime=atoi(sp[6]);
			ms->delay=atoi(sp[7]);
			if (battle_config.mob_skill_delay != 100)
				ms->delay = ms->delay*battle_config.mob_skill_delay/100;
			ms->cancel=atoi(sp[8]);
			if( strcmp(sp[8],"yes")==0 ) ms->cancel=1;
			ms->target=atoi(sp[9]);
			for(j=0;j<sizeof(target)/sizeof(target[0]);j++){
				if( strcmp(sp[9],target[j].str)==0)
					ms->target=target[j].id;
			}
			ms->cond1=-1;
			for(j=0;j<sizeof(cond1)/sizeof(cond1[0]);j++){
				if( strcmp(sp[10],cond1[j].str)==0)
					ms->cond1=cond1[j].id;
			}
			ms->cond2=atoi(sp[11]);
			for(j=0;j<sizeof(cond2)/sizeof(cond2[0]);j++){
				if( strcmp(sp[11],cond2[j].str)==0)
					ms->cond2=cond2[j].id;
			}
			ms->val[0]=atoi(sp[12]);
			ms->val[1]=atoi(sp[13]);
			ms->val[2]=atoi(sp[14]);
			ms->val[3]=atoi(sp[15]);
			ms->val[4]=atoi(sp[16]);
			if(sp[17] != NULL && strlen(sp[17])>2)
				ms->emotion=atoi(sp[17]);
			else
				ms->emotion=-1;
			mob_db[mob_id].maxskill=i+1;
		}
		fclose(fp);
		ShowStatus("Done reading '"CL_WHITE"%s"CL_RESET"'.\n",filename[x]);
	}
	return 0;
}
/*==========================================
 * db/mob_race_db.txt reading
 *------------------------------------------
 */
int mob_readdb_race(void)
{
	FILE *fp;
	char line[1024];
	int race,j,k;
	char *str[20],*p,*np;

	if( (fp=safefopen("db/mob_race2_db.txt","r"))==NULL ){
		ShowError("can't read db/mob_race2_db.txt\n");
		return -1;
	}

	while(fgets(line,1020,fp)){
		if( !skip_empty_line(line) )
			continue;
		memset(str,0,sizeof(str));

		for(j=0,p=line;j<12;j++){
			if((np=strchr(p,','))!=NULL){
				str[j]=p;
				*np=0;
				p=np+1;
			} else
				str[j]=p;
		}
		if(str[0]==NULL)
			continue;

		race=atoi(str[0]);
		if (race < 0 || race >= MAX_MOB_RACE_DB)
			continue;

		for (j=1; j<20; j++) {
			if (!str[j])
				break;
			k=atoi(str[j]);
			if (k < 1000 || k > MAX_MOB_DB)
				continue;
			mob_db[k].race2 = race;
			//mob_race_db[race][j] = k;
		}
	}
	fclose(fp);
	ShowStatus("Done reading '"CL_WHITE"%s"CL_RESET"'.\n","db/mob_race2_db.txt");

	return 0;
}



#ifndef TXT_ONLY
/*==========================================
 * SQL reading
 *------------------------------------------
 */
int mob_read_sqldb(void)
{
	char line[1024];
	int i, j, class_;
	double exp, maxhp;
	unsigned long ln=0;
	char *str[60], *p, *np; // 55->60 Lupus
	char *mob_db_name[] = { mob_db_db, mob_db2_db };

	memset(mob_db,0,sizeof(mob_db));

    for (i = 0; i < 2; i++)
	{
		sprintf (tmp_sql, "SELECT * FROM `%s`", mob_db_name[i]);
		if(mysql_SendQuery(mysql_handle, tmp_sql) )
		{
			ShowError("DB server Error (select %s to Memory)- %s\n", mob_db_name[i], mysql_error(mysql_handle));
		}
		sql_res = mysql_store_result(mysql_handle);
		if (sql_res)
		{
			while((sql_row = mysql_fetch_row(sql_res)))
			{
				sprintf(line,"%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"
							"%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"
							"%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s",
							sql_row[0],sql_row[1],sql_row[2],sql_row[3],sql_row[4],
							sql_row[5],sql_row[6],sql_row[7],sql_row[8],sql_row[9],
							sql_row[10],sql_row[11],sql_row[12],sql_row[13],sql_row[14],
							sql_row[15],sql_row[16],sql_row[17],sql_row[18],sql_row[19],
							sql_row[20],sql_row[21],sql_row[22],sql_row[23],sql_row[24],
							sql_row[25],sql_row[26],sql_row[27],sql_row[28],sql_row[29],
							sql_row[30],sql_row[31],sql_row[32],sql_row[33],sql_row[34],
							sql_row[35],sql_row[36],sql_row[37],sql_row[38],sql_row[39],
							sql_row[40],sql_row[41],sql_row[42],sql_row[43],sql_row[44],
							sql_row[45],sql_row[46],sql_row[47],sql_row[48],sql_row[49],
							sql_row[50],sql_row[51],sql_row[52],sql_row[53],sql_row[54],
							sql_row[55],sql_row[56]);

				for (j = 0, p = line; j < 57; j++)
				{
					if((np=strchr(p,','))!=NULL)
					{
						str[j] = p;
						*np = 0;
						p = np+1;
					}
					else
						str[j] = p;
				}
				class_ = atoi(str[0]);

				if (class_ <= 1000 || class_ > MAX_MOB_DB)
					continue;

				ln++;

				mob_db[class_].view_class = class_;
				memcpy(mob_db[class_].name, str[1], 24);
				memcpy(mob_db[class_].jname, str[2], 24);
				mob_db[class_].lv = atoi(str[3]);
				mob_db[class_].max_hp = atoi(str[4]);
				mob_db[class_].max_sp = atoi(str[5]);

				exp = atof(str[6]) * battle_config.base_exp_rate / 100;
				if (exp < 0) exp = 0;
				else if (exp > 0x7fffffff) exp = 0x7fffffff;
				mob_db[class_].base_exp = (int)exp;

				exp = atof(str[7]) * (double)battle_config.job_exp_rate / 100;
				if (exp < 0) exp = 0;
				else if (exp > 0x7fffffff) exp = 0x7fffffff;
				mob_db[class_].job_exp = (int)exp;

				mob_db[class_].range = atoi(str[8]);
				mob_db[class_].atk1 = atoi(str[9]);
				mob_db[class_].atk2 = atoi(str[10]);
				mob_db[class_].def = atoi(str[11]);
				mob_db[class_].mdef = atoi(str[12]);
				mob_db[class_].str = atoi(str[13]);
				mob_db[class_].agi = atoi(str[14]);
				mob_db[class_].vit = atoi(str[15]);
				mob_db[class_].int_ = atoi(str[16]);
				mob_db[class_].dex = atoi(str[17]);
				mob_db[class_].luk = atoi(str[18]);
				mob_db[class_].range2 = atoi(str[19]);
				mob_db[class_].range3 = atoi(str[20]);
				mob_db[class_].size = atoi(str[21]);
				mob_db[class_].race = atoi(str[22]);
				mob_db[class_].element = atoi(str[23]);
				mob_db[class_].mode = atoi(str[24]);
				mob_db[class_].speed = atoi(str[25]);
				mob_db[class_].adelay = atoi(str[26]);
				mob_db[class_].amotion = atoi(str[27]);
				mob_db[class_].dmotion = atoi(str[28]);

				for (j = 0; j < 10; j++)
				{	// 8 -> 10 Lupus
					int rate = 0, type, ratemin, ratemax;
					mob_db[class_].dropitem[j].nameid=atoi(str[29+j*2]);
					type = itemdb_type(mob_db[class_].dropitem[j].nameid);
					if (type == 0)
					{	// Added by Valaris
						rate = battle_config.item_rate_heal * atoi(str[30+j*2]) / 100;
						ratemin = battle_config.item_drop_heal_min;
						ratemax = battle_config.item_drop_heal_max;
					}
					else if (type == 2)
					{
						rate = battle_config.item_rate_use * atoi(str[30+j*2]) / 100;
						ratemin = battle_config.item_drop_use_min;
						ratemax = battle_config.item_drop_use_max;	// End
					}
					else if (type == 4 || type == 5 || type == 8)
					{	// Changed to include Pet Equip
						rate = battle_config.item_rate_equip * atoi(str[30+j*2]) / 100;
						ratemin = battle_config.item_drop_equip_min;
						ratemax = battle_config.item_drop_equip_max;
					}
					else if (type == 6)
					{
						rate = battle_config.item_rate_card * atoi(str[30+j*2]) / 100;
						ratemin = battle_config.item_drop_card_min;
						ratemax = battle_config.item_drop_card_max;
					}
					else
					{
						rate = battle_config.item_rate_common * atoi(str[30+j*2]) / 100;
						ratemin = battle_config.item_drop_common_min;
						ratemax = battle_config.item_drop_common_max;
					}
					mob_db[class_].dropitem[j].p = (rate < ratemin) ? ratemin : (rate > ratemax) ? ratemax: rate;
				}
				// MVP EXP Bonus, Chance: MEXP,ExpPer
				mob_db[class_].mexp = atoi(str[49]) * battle_config.mvp_exp_rate / 100;
				mob_db[class_].mexpper = atoi(str[50]);
				//Now that we know if it is an mvp or not,
				//apply battle_config modifiers [Skotlex]
				maxhp = (double)mob_db[class_].max_hp;
				if (mob_db[class_].mexp > 0)
				{	//Mvp
					if (battle_config.mvp_hp_rate != 100)
						maxhp = maxhp * (double)battle_config.mvp_hp_rate /100.;
				} else if (battle_config.monster_hp_rate != 100) //Normal mob
					maxhp = maxhp * (double)battle_config.monster_hp_rate /100.;
				if (maxhp < 0) maxhp = 1;
				else if (maxhp > 0x7fffffff) maxhp = 0x7fffffff;
				mob_db[class_].max_hp = (int)maxhp;

				// MVP Drops: MVP1id,MVP1per,MVP2id,MVP2per,MVP3id,MVP3per
				for (j = 0; j < 3; j++)
				{
					mob_db[class_].mvpitem[j].nameid = atoi(str[51+j*2]);
					mob_db[class_].mvpitem[j].p = atoi(str[52+j*2]) * battle_config.mvp_item_rate / 100;
				}
				for (j = 0; j < MAX_RANDOMMONSTER; j++)
					mob_db[class_].summonper[j] = 0;
				mob_db[class_].maxskill = 0;

				mob_db[class_].sex = 0;
				mob_db[class_].hair = 0;
				mob_db[class_].hair_color = 0;
				mob_db[class_].weapon = 0;
				mob_db[class_].shield = 0;
				mob_db[class_].head_top = 0;
				mob_db[class_].head_mid = 0;
				mob_db[class_].head_buttom = 0;
			}
			mysql_free_result(sql_res);
			ShowStatus("Done reading '"CL_WHITE"%lu"CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n", ln, mob_db_name[i]);
			ln = 0;
		}
	}
	return 0;
}
#endif /* not TXT_ONLY */

void mob_reload(void)
{
#ifndef TXT_ONLY
    if(db_use_sqldbs)
        mob_read_sqldb();
    else
#endif /* TXT_ONLY */
	mob_readdb();

	mob_readdb_mobavail();
	mob_read_randommonster();
	mob_readskilldb();
	mob_readdb_race();
}


/*==========================================
 * Circumference initialization of mob
 *------------------------------------------
 */
int do_init_mob(void)
{
#ifndef TXT_ONLY
    if(db_use_sqldbs)
        mob_read_sqldb();
    else
#endif /* TXT_ONLY */
        mob_readdb();

	mob_readdb_mobavail();
	mob_read_randommonster();
	mob_readskilldb();
	mob_readdb_race();

	add_timer_func_list(mob_timer,"mob_timer");
	add_timer_func_list(mob_delayspawn,"mob_delayspawn");
	add_timer_func_list(mob_delay_item_drop,"mob_delay_item_drop");
	add_timer_func_list(mob_delay_item_drop2,"mob_delay_item_drop2");
	add_timer_func_list(mob_ai_hard,"mob_ai_hard");
	add_timer_func_list(mob_ai_lazy,"mob_ai_lazy");
	add_timer_func_list(mobskill_castend_id,"mobskill_castend_id");
	add_timer_func_list(mobskill_castend_pos,"mobskill_castend_pos");
	add_timer_func_list(mob_timer_delete,"mob_timer_delete");
	add_timer_interval(gettick()+MIN_MOBTHINKTIME,MIN_MOBTHINKTIME,mob_ai_hard,0,0);
	add_timer_interval(gettick()+MIN_MOBTHINKTIME*10,MIN_MOBTHINKTIME*10,mob_ai_lazy,0,0);

	return 0;
}