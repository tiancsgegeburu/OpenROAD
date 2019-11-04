// OpenStaDB, OpenSTA on OpenDB
// Copyright (c) 2019, Parallax Software, Inc.
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "Machine.hh"
#include "sta_db/DbNetwork.hh"
#include "DbSdcNetwork.hh"
#include "sta_db/StaDb.hh"
#include "opendb/db.h"

namespace sta {

StaDb::StaDb(dbDatabase *db) :
  Sta(),
  db_(db)
{
}


void
StaDb::initNetwork()
{
  dbNetwork()->init(db_);
}

DbNetwork *
StaDb::dbNetwork()
{
  return dynamic_cast<DbNetwork *>(network_);
}

void
StaDb::makeNetwork()
{
  network_ = new DbNetwork();
}

void
StaDb::makeSdcNetwork()
{
  sdc_network_ = new DbSdcNetwork(network_);
}

}
