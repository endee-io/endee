// Endee — high-performance vector database
// Copyright (C) 2026 Endee Labs
//
// Portions of this file are derived from hnswlib (https://github.com/nmslib/hnswlib),
// originally licensed under the Apache License 2.0. As incorporated and modified
// here, the combined work is distributed under the GNU Affero General Public License.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <mutex>
#include <string.h>
#include <deque>

namespace hnswlib {
    typedef unsigned short int vl_type;

    class VisitedList {
    public:
        vl_type curV;
        vl_type* mass;
        unsigned int numelements;

        VisitedList(int numelements1) {
            curV = -1;
            numelements = numelements1;
            mass = new vl_type[numelements];
        }

        void reset() {
            curV++;
            if(curV == 0) {
                memset(mass, 0, sizeof(vl_type) * numelements);
                curV++;
            }
        }

        ~VisitedList() { delete[] mass; }
    };
    ///////////////////////////////////////////////////////////
    //
    // Class for multi-threaded pool-management of VisitedLists
    //
    /////////////////////////////////////////////////////////

    class VisitedListPool {
        std::deque<VisitedList*> pool;
        std::mutex poolguard;
        int numelements;

    public:
        VisitedListPool(int initmaxpools, int numelements1) {
            numelements = numelements1;
            for(int i = 0; i < initmaxpools; i++) {
                pool.push_front(new VisitedList(numelements));
            }
        }

        VisitedList* getFreeVisitedList() {
            VisitedList* rez;
            {
                std::unique_lock<std::mutex> lock(poolguard);
                if(pool.size() > 0) {
                    rez = pool.front();
                    pool.pop_front();
                } else {
                    rez = new VisitedList(numelements);
                }
            }
            rez->reset();
            return rez;
        }

        void releaseVisitedList(VisitedList* vl) {
            std::unique_lock<std::mutex> lock(poolguard);
            pool.push_front(vl);
        }

        ~VisitedListPool() {
            while(pool.size()) {
                VisitedList* rez = pool.front();
                pool.pop_front();
                delete rez;
            }
        }
    };
}  // namespace hnswlib
