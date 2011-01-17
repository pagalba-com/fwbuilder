/* 

                          Firewall Builder

                 Copyright (C) 2010 NetCitadel, LLC

  Author:  Vadim Kurland     vadim@vk.crocodile.org

  $Id$

  This program is free software which we release under the GNU General Public
  License. You may redistribute and/or modify this program under the terms
  of that license as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
 
  To get a copy of the GNU General Public License, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#include "config.h"

#include "NamedObjectsAndGroupsSupport.h"
#include "ObjectGroupFactory.h"

#include "fwbuilder/FWObjectDatabase.h"
#include "fwbuilder/RuleElement.h"
#include "fwbuilder/IPService.h"
#include "fwbuilder/ICMPService.h"
#include "fwbuilder/TCPService.h"
#include "fwbuilder/UDPService.h"
#include "fwbuilder/CustomService.h"
#include "fwbuilder/Network.h"
#include "fwbuilder/Policy.h"
#include "fwbuilder/Interface.h"
#include "fwbuilder/Management.h"
#include "fwbuilder/Resources.h"
#include "fwbuilder/AddressTable.h"
#include "fwbuilder/Firewall.h"

#include "fwcompiler/Compiler.h"

#include <iostream>
#include <algorithm>

#include <assert.h>

#include <QString>
#include <QStringList>


using namespace libfwbuilder;
using namespace fwcompiler;
using namespace std;


Group* CreateObjectGroups::object_groups = NULL;
map<int, NamedObject*> NamedObjectManager::named_objects;


NamedObjectManager::NamedObjectManager(const libfwbuilder::Firewall *_fw)
{
    fw = _fw;
}

NamedObjectManager::~NamedObjectManager()
{
    std::map<int, NamedObject*>::iterator it1;
    for (it1=named_objects.begin(); it1!=named_objects.end(); ++it1)
    {
        delete it1->second;
    }
    named_objects.clear();
}

string NamedObjectManager::addNamedObject(const FWObject *obj)
{
    string res;
    if (BaseObjectGroup::constcast(obj)!=NULL)
    {
        for (FWObject::const_iterator i=obj->begin(); i!=obj->end(); ++i)
        {
            res += addNamedObject(FWReference::getObject(*i));
        }
        return res;
    }
    if (named_objects[obj->getId()] == NULL)
    {
        NamedObject *asa8obj = new NamedObject(obj);
        res = asa8obj->getCommand(fw).toStdString();
        named_objects[obj->getId()] = asa8obj;
    }
    return res;
}

NamedObject* NamedObjectManager::getNamedObject(const FWObject *obj)
{
    return named_objects[obj->getId()];
}



void CreateObjectGroups::init(FWObjectDatabase *db)
{
    object_groups = new Group();
    db->add( object_groups );
    //if (named_objects.size() > 0) clearNamedObjectsRegistry();
}

CreateObjectGroups::~CreateObjectGroups()
{
}

BaseObjectGroup* CreateObjectGroups::findObjectGroup(RuleElement *re)
{
    list<FWObject*> relement;

    for (FWObject::iterator i1=re->begin(); i1!=re->end(); ++i1) 
    {
        FWObject *o   = *i1;
        FWObject *obj = FWReference::getObject(o);
        relement.push_back(obj);
    }


    for (FWObject::iterator i=object_groups->begin(); i!=object_groups->end(); ++i)
    {
        BaseObjectGroup *og=dynamic_cast<BaseObjectGroup*>(*i);
        assert(og!=NULL);

        if (og->size()==0 || (og->size()!=re->size()) ) continue;

        bool match=true;
        for (FWObject::iterator i1=og->begin(); i1!=og->end(); ++i1) 
        {
            FWObject *o   = *i1;
            FWObject *obj = o;
            if (FWReference::cast(o)!=NULL) obj=FWReference::cast(o)->getPointer();

            if ( find(relement.begin(), relement.end(), obj)==relement.end() )
            {
                match=false;
                break;
            }
        }
        if (match) return og;
    }
    return NULL;
}

bool CreateObjectGroups::processNext()
{
    Rule *rule = prev_processor->getNextRule(); if (rule==NULL) return false;
    string version = compiler->fw->getStr("version");
    string platform = compiler->fw->getStr("platform");

    Interface *rule_iface = Interface::cast(compiler->dbcopy->findInIndex(
                                                rule->getInterfaceId()));
    assert(rule_iface);

    RuleElement *re = RuleElement::cast(rule->getFirstByType(re_type));


    /*
     * If rule element holds just one object, then there is no need to create
     * object group. However if this one object is CustomService, then we
     * should create the group anyway.
     */
    if (re->size()==1)
    {
        if (XMLTools::version_compare(version, "8.3")>=0)
        {
            FWObject *obj = FWReference::getObject(re->front());
            if (!CustomService::isA(obj))
            {
                tmp_queue.push_back(rule);
                return true;
            }
        } else
        {
            tmp_queue.push_back(rule);
            return true;
        }
    }

    bool supports_mixed_groups =
        Resources::platform_res[platform]->getResourceBool(
            string("/FWBuilderResources/Target/options/") +
            "version_" + version + "/supports_mixed_service_groups");

    BaseObjectGroup *obj_group = findObjectGroup(re);
    if (obj_group==NULL)
    {
        //obj_group= new BaseObjectGroup();
        obj_group = ObjectGroupFactory::createObjectGroup(compiler->fw);

        FWObject *obj = FWReference::getObject(re->front());
        BaseObjectGroup::object_group_type og_type =
            obj_group->getObjectGroupTypeFromFWObject(obj);
        obj_group->setObjectGroupType(og_type);

        if (obj_group->isServiceGroup() && supports_mixed_groups && re->size() > 1)
        {
            // rule element contains >1 object, check if they are of different types
            for (FWObject::iterator i1=re->begin(); i1!=re->end(); ++i1) 
            {
                FWObject *obj = FWReference::getObject(*i1);
                if (og_type != obj_group->getObjectGroupTypeFromFWObject(obj))
                {
                    obj_group->setObjectGroupType(BaseObjectGroup::MIXED_SERVICE);
                    break;
                }
            }
        }

        QStringList gn;
        if (!rule_iface->getLabel().empty())
            gn.push_back(rule_iface->getLabel().c_str());

        gn.push_back(rule->getUniqueId().c_str());
        gn.push_back(name_suffix.c_str());
        obj_group->setName(gn.join(".").toStdString());

        object_groups->add(obj_group);

        packObjects(re, obj_group);

    } else
    {
        re->clearChildren(false); //do not want to destroy children objects
        re->addRef(obj_group);
    }


//    assert(re->size()==1);

    tmp_queue.push_back(rule);
    return true;
}

void CreateObjectGroups::packObjects(RuleElement *re, BaseObjectGroup *obj_group)
{
    for (FWObject::iterator i1=re->begin(); i1!=re->end(); ++i1) 
    {
        FWObject *o = *i1;
        FWObject *obj = o;
        if (FWReference::cast(o)!=NULL)
            obj = FWReference::cast(o)->getPointer();
        obj_group->addRef(obj);
    }
    re->clearChildren(false); //do not want to destroy children objects
    re->addRef(obj_group);
}

void CreateObjectGroupsForTSrc::packObjects(RuleElement *re,
                                            BaseObjectGroup *obj_group)
{
    if (libfwbuilder::XMLTools::version_compare(
            compiler->fw->getStr("version"), "8.3")>=0)
    {
        // put all objects inside of the group, except for the interface
        // if it belongs to the firewall
        FWObject *re_interface = NULL;
        for (FWObject::iterator i1=re->begin(); i1!=re->end(); ++i1) 
        {
            FWObject *o = *i1;
            FWObject *obj = o;
            if (FWReference::cast(o)!=NULL)
                obj = FWReference::cast(o)->getPointer();
            if (Interface::isA(obj) && obj->isChildOf(compiler->fw))
            {
                re_interface = obj;
                continue;
            }
            obj_group->addRef(obj);
        }
        re->clearChildren(false); //do not want to destroy children objects
        if (re_interface)
        {
            // add interface back.
            re->addRef(re_interface);
        }
        re->addRef(obj_group);
    } else
    {
        CreateObjectGroups::packObjects(re, obj_group);
    }
}

bool printObjectGroups::processNext()
{
    slurp();
    if (tmp_queue.size()==0) return false;

    for (FWObject::iterator i=CreateObjectGroups::object_groups->begin();
         i!=CreateObjectGroups::object_groups->end(); ++i)
    {
        BaseObjectGroup *og = dynamic_cast<BaseObjectGroup*>(*i);
        assert(og!=NULL);
        if (og->size()==0) continue;
        compiler->output << endl;
        try
        {
            compiler->output << og->toString(named_objects_manager);
        } catch (FWException &ex)
        {
            compiler->abort(ex.toString());
        }
    }

    return true;
}

void printNamedObjectsCommon::printObjectsForRE(RuleElement *re)
{
    if (re->isAny()) return;

    for (FWObject::iterator it=re->begin(); it!=re->end(); ++it)
    {
        FWObject *obj = FWReference::getObject(*it);
        if (Interface::isA(obj)) continue;
        compiler->output << named_objects_manager->addNamedObject(obj);
    }
}

bool printNamedObjectsForPolicy::haveCustomService(FWObject *grp)
{
    for (FWObject::iterator it=grp->begin(); it!=grp->end(); ++it)
    {
        FWObject *obj = FWReference::getObject(*it);
        if (BaseObjectGroup::constcast(obj)!=NULL)
        {
            if (haveCustomService(obj)) return true;
        } else
        {
            if (CustomService::isA(obj)) return true;
        }
    }
    return false;
}

bool printNamedObjectsForPolicy::processNext()
{
    slurp();
    if (tmp_queue.size()==0) return false;

    compiler->output << endl;

    for (deque<Rule*>::iterator k=tmp_queue.begin(); k!=tmp_queue.end(); ++k) 
    {
        PolicyRule *policy_rule = PolicyRule::cast( *k );
        if (policy_rule)
        {
            // At this time, we only need object groups in policy rules
            // when CustomService object is used in Service

            // RuleElementSrc *src_re = policy_rule->getSrc();  assert(src_re);
            // printObjectsForRE(src_re);
            // RuleElementDst *dst_re = policy_rule->getDst();  assert(dst_re);
            // printObjectsForRE(dst_re);

            RuleElementSrv *srv_re = policy_rule->getSrv();  assert(srv_re);
            if (haveCustomService(srv_re)) printObjectsForRE(srv_re);
        }
    }

    return true;
}


bool printNamedObjectsForNAT::processNext()
{
    slurp();
    if (tmp_queue.size()==0) return false;

    compiler->output << endl;

    for (deque<Rule*>::iterator k=tmp_queue.begin(); k!=tmp_queue.end(); ++k) 
    {
        NATRule *nat_rule = NATRule::cast( *k );
        if (nat_rule)
        {
            RuleElementOSrc *osrc_re = nat_rule->getOSrc();  assert(osrc_re);
            printObjectsForRE(osrc_re);

            RuleElementODst *odst_re = nat_rule->getODst();  assert(odst_re);
            printObjectsForRE(odst_re);

            RuleElementOSrv *osrv_re = nat_rule->getOSrv();  assert(osrv_re);
            printObjectsForRE(osrv_re);

            RuleElementTSrc *tsrc_re = nat_rule->getTSrc();  assert(tsrc_re);
            printObjectsForRE(tsrc_re);

            RuleElementTDst *tdst_re = nat_rule->getTDst();  assert(tdst_re);
            printObjectsForRE(tdst_re);

            RuleElementTSrv *tsrv_re = nat_rule->getTSrv();  assert(tsrv_re);
            printObjectsForRE(tsrv_re);
        }

    }

    return true;
}
