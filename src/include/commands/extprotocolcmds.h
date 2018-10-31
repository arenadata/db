/*-------------------------------------------------------------------------
 *
 * extprotocolcmds.h
 *	  prototypes for extprotocolcmds.c.
 *
 *-------------------------------------------------------------------------
 */

#ifndef EXTPROTOCOLCMDS_H
#define EXTPROTOCOLCMDS_H

#include "nodes/pg_list.h"

extern void DefineExtProtocol(List *name, List *parameters, bool trusted);
extern void RemoveExtProtocolById(Oid protOid);
extern Oid AlterExtProtocolOwner(const char *name, Oid newOwnerId);


#endif   /* EXTPROTOCOLCMDS_H */
