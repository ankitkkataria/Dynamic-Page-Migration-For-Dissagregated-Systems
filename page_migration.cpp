
#define page_swap_time 1000 // per-page swap (background) time (In nano seconds)
#define tlb_miss_time 50

int64_t background_pg_swap_clock[num_nodes];
int64_t resume_stall_clock[num_nodes] = {0};
int64_t last_migration_cycle[num_nodes] = {0};

// #define initiliastion_epoch_length 1e6 // For initilaisng page migration parameters for the next epoch

bool migration_flag[num_nodes] = {0};
bool migrate_stall[num_nodes] = {0};
uint64_t local_victim_paddr[num_nodes]; // stores the local victim's physical address for each node.
uint64_t remote_hot_paddr[num_nodes];   // stores the remote hot page's physical address for each node.
double hot_page_threshold[num_nodes] = {30};
double cold_page_threshold[num_nodes] = {10};
double average_positive_migration_revenue[num_nodes] = {0};
double average_negative_migration_revenue[num_nodes] = {0};
double sum_of_all_negative_migration_revenues = 0;
double sum_of_all_positive_migration_revenues = 0;
double number_of_negative_migration_revenues = 0;
double number_of_positive_migration_revenues = 0;
double remote_mem_accesses_time = 0;
uint64_t access_count_global[num_nodes] = {0};                             // for each node their global access count will be stored here.
unordered_map<uint64_t, uint64_t> access_count_page[num_nodes];            // array of hashmaps where each hashmap contains the access count of each of the pages.
unordered_map<uint64_t, double> local_plus_remote_page_hotness[num_nodes]; // array that contains all the page's (both local and remote) and their corresponding hotnesses.

#define migration_benefit_per_mem_access 200 // number of cycles saved by migrating page to local from remote per memory access.
#define TLB_shootdown_cycles 4000            // denotes the cycles it takes to perform a TLB shootdown.

// calculate overhead of migration to compare it later with migration benefits

// store the number of migrations performed during the whole simulation on a node
int16_t migration_counter[num_nodes] = {0}; // this will store the total number of migrations performed for each of the nodes.

unordered_map<uint64_t, uint64_t> pages_migrated_in_feedback_cycle[num_nodes]; // feedback cycle

uint64_t update_migration_benefit(int node_id, uint64_t page_accessed)
{
    if (pages_migrated_in_feedback_cycle[node_id].find(page_accessed) != pages_migrated_in_feedback_cycle[node_id].end())
    {
        pages_migrated_in_feedback_cycle[node_id][page_accessed]++; // it counts the number of times this particular page has been accessed so we could use it while computing the migration benefit or revenue.
    }
}

// THIS WILL BE CALLED AFTER A PAGE HAS BEEN MIGRATED NOT AFTER EVERY MEMORY ACCESS.
void update_params(int node_id, uint64_t count)
{
    uint64_t revenue = count * 200;                            // here 200 is actually the time we save if one access happened from local memory rather than global memory.
    revenue = revenue - TLB_shootdown_cycles - page_swap_time - remote_mem_accesses_time - tlb_miss_time; // again subtracting the page_swap_time and TLB_shootdown_cycles.
    if (revenue < 0)                                           // if the revenue is less than zero that means what we lost by those pages staying there in the remote memory is nothing if they would have been in local memory it would only degrade our performance.
    {
        if (revenue < average_negative_migration_revenue[node_id])
        {
            hot_page_threshold[node_id] += 2;
            cold_page_threshold[node_id] -= 2;
        }
        else
        {
            // so just increment the hot page threshold to see the invalid migrations just comment this line out and put a global counter here and that will give you the number of invalid migrations.
            hot_page_threshold[node_id] += 1;
            cold_page_threshold[node_id] -= 1;
        }

        number_of_negative_migration_revenues++;
        sum_of_all_negative_migration_revenues += revenue;
        average_negative_migration_revenue = sum_of_all_negative_migration_revenues / number_of_negative_migration_revenues;
    }
    else
    {
        if (revenue > average_positive_migration_revenue[node_id])
        {
            hot_page_threshold[node_id] -= 2;
            cold_page_threshold[node_id] += 2;
        }
        else
        {
            hot_page_threshold[node_id] -= 1;
            cold_page_threshold[node_id] += 1;
        }

        number_of_positive_migration_revenues++;
        sum_of_all_positive_migration_revenues += revenue;
        average_positive_migration_revenue = sum_of_all_positive_migration_revenues / number_of_positive_migration_revenues;
    }

    if (hot_page_threshold[node_id] < 20)
    {
        hot_page_threshold[node_id] = 20;
    }
}

// get victim page list

int pgd_ptr = 0;
int pud_ptr = 0;
int pmd_ptr = 0;
int pte_ptr = 0;

// the code in the function below is how we find a victim page in the local memory
// return a victim page paddr to be added in victim list (if available)
int64_t traverse_page_table(pgd &_pgd, int node_id)
{
    pud *_pud;
    pmd *_pmd;
    pte *_pte;
    page *_page;

    for (int i = 0; i < 512; i++)
    {
        _pud = _pgd.access_in_pgd(pgd_ptr);

        if (_pud == nullptr)
            goto pud;

        for (int j = 0; j < 512; j++)
        {
            _pmd = _pud->access_in_pud(pud_ptr);

            if (_pmd == nullptr)
                goto pmd;

            for (int k = 0; k < 512; k++)
            {
                _pte = _pmd->access_in_pmd(pmd_ptr);

                if (_pte == nullptr)
                    goto pte;

                for (int l = 0; l < 512; l++)
                {
                    _page = _pte->access_in_pte(pte_ptr);

                    if (_page != nullptr)
                    {
                        long unsigned paddr = _page->get_page_physical_addr();
                        bool r_bit, t_bit, v_bit; // r_bit will store the reference bit and t_bit will store the TLB_present_bit and v_bit will store the in_victim_list_bit

                        r_bit = _page->referenced_bit;
                        t_bit = _page->TLB_present_bit;
                        v_bit = _page->in_victim_list_bit;

                        // check if this page is local, only then we select it as the victim page
                        // using TLB aware clock replacement

                        if (L[node_id].is_local(paddr))
                        {
                            if (r_bit == 0 && t_bit == 0 && v_bit == 0)
                            {
                                // select this page as victim and its victim bit
                                _page->referenced_bit = 0;
                                _page->TLB_present_bit = 0;
                                _page->in_victim_list_bit = 1;

                                return paddr;
                            }
                            else if (r_bit == 0 && t_bit == 0 && v_bit == 1)
                            {
                                // already present in victim, skip
                            }
                            else if (r_bit == 0 && t_bit == 1 && v_bit == 0)
                            {
                                // unreachable state
                                // r_bit cannot be zero if t_bit is 1
                            }
                            else if (r_bit == 0 && t_bit == 1 && v_bit == 1)
                            {
                                // unreachable state
                                // r_bit cnnot be zero if t_bit is 1, v_bit becomes insignificant
                            }
                            else if (r_bit == 1 && t_bit == 0 && v_bit == 0)
                            {
                                // give second chance to this page before selecting it as victim
                                _page->referenced_bit = 0;
                            }
                            else if (r_bit == 1 && t_bit == 0 && v_bit == 1)
                            {
                                // unreachable state
                                // already in victim list, r_bit cannot be 1
                            }
                            else if (r_bit == 1 && t_bit == 1 && v_bit == 0)
                            {
                                // present in TLB, skip this page
                                // and don't yet implement second-chance logic
                                _page->TLB_present_bit = 0;
                            }
                            else if (r_bit == 1 && t_bit == 1 && v_bit == 1)
                            {
                                // unreachable state
                                // cannot be in victim list, if it is refereenced and in TLB
                            }
                        }
                    }
                    pte_ptr = (pte_ptr + 1) % 512; // this incremenent is done so we can move to the next page if we don't find the victim page in the previous page we were looking in.
                }
            pte:
                pmd_ptr = (pmd_ptr + 1) % 512; // similarly this is so we could move to the next pte in the page table one level above the pages.
            }
        pmd:
            pud_ptr = (pud_ptr + 1) % 512; // same thing as above just more level up.
        }
    pud:
        pgd_ptr = (pgd_ptr + 1) % 512; // same thing as above just more level up.
    }
    return -1;
}

// finds the victim page for a node.
int64_t get_victim_page(int node_id)
{
    int64_t paddr;
    paddr = L[node_id].find_unallocated_page();
    if (paddr != -1)
    {
        return paddr;
    }

    paddr = traverse_page_table(_pgd[node_id], node_id); // take the outermost page table that this node has and move across all the pages that can be taken as a victim page.

    while (paddr == -1)
    {
        // if(common_clock>30900000) cout<<"get_vctim2"<<endl;
        paddr = traverse_page_table(_pgd[node_id], node_id);
    }

    return paddr;
}

bool update_page_bits(pgd &_pgd, uint64_t vaddr, uint64_t paaddr, int node_id, bool is_victim) // this function updates the page's bits depending on whether it's a victim page or not.
{
    pud *_pud;
    pmd *_pmd;
    pte *_pte;
    page *_page;

    unsigned long pgd_vaddr = 0L, pud_vaddr = 0L, pmd_vaddr = 0L, pte_vaddr = 0L, page_offset_addr = 0L;
    split_vaddr(pgd_vaddr, pud_vaddr, pmd_vaddr, pte_vaddr, page_offset_addr, vaddr);

    _pud = _pgd.access_in_pgd(pgd_vaddr);
    if (_pud == nullptr)
    {
        cout << "error in update_victim_tlb_bits, pud not found";
        return false;
    }
    else if (_pud != nullptr)
    {
        _pmd = _pud->access_in_pud(pud_vaddr);
        if (_pmd == nullptr)
        {
            cout << "error in update_victim_tlb_bits, pmd not found";
            return false;
        }
        else if (_pmd != nullptr)
        {
            _pte = _pmd->access_in_pmd(pmd_vaddr);
            if (_pte == nullptr)
            {
                cout << "error in update_victim_tlb_bits, pte not found";
                return false;
            }
            else if (_pte != nullptr)
            {
                _page = _pte->access_in_pte(pte_vaddr);
                if (_page == nullptr)
                {
                    cout << "error in update_victim_tlb_bits, page not found";
                    return false;
                }
                else if (_page != nullptr)
                {

                    if (is_victim)
                    {
                        _page->TLB_present_bit = 0;
                    }
                    else
                    {
                        _page->referenced_bit = 1;
                        _page->TLB_present_bit = 1;
                        _page->in_victim_list_bit = 0;
                        uint64_t paddr = _page->get_page_physical_addr();
                    }

                    return true;
                }
            }
        }
    }
}

void update_hash_table_access_count(uint64_t paddr, int node_id, int is_remote)
{
    if (access_count_page[node_id].find(paddr) == access_count_page[node_id].end())
    {
        access_count_page[node_id][paddr] = 0; // if a page is being accessed for the first time then just set the access_count_page = 0 for now.
    }
    if (local_plus_remote_page_hotness[node_id].find(paddr) == local_plus_remote_page_hotness[node_id].end())
    {
        local_plus_remote_page_hotness[node_id][paddr] = 1.0; // and set it's hotness to 1.0
    }
    // cout<<"\nnum_loc: "<<L[node_id].num_local_pages();
    // cin.get();
    double d = (double)(local_plus_remote_page_hotness[node_id][paddr]) / (local_plus_remote_page_hotness[node_id][paddr] + (access_count_global[node_id] - access_count_page[node_id][paddr]) / L[node_id].num_local_pages());
    local_plus_remote_page_hotness[node_id][paddr] = local_plus_remote_page_hotness[node_id][paddr] * d + 1;
    access_count_global[node_id]++;
    access_count_page[node_id][paddr] = access_count_global[node_id];
    if (common_clock % 100000 == 0)
    {
        cout << "\nHotness: " << local_plus_remote_page_hotness[node_id][paddr];
    }
    if (local_plus_remote_page_hotness[node_id][paddr] >= hot_page_threshold[node_id] && is_remote)
    {
        int64_t temp = get_victim_page(node_id);
        remote_hot_paddr[node_id] = paddr;
        local_victim_paddr[node_id] = temp;
        //  cout<<"\nready to migrate at cycle "<<common_clock;
        page_migration << "\nready to migrate at cycle " << common_clock;
        migration_flag[node_id] = 1; // do the swapping in background and still continue with current local and remote addresses for pages to migrate
        background_pg_swap_clock[node_id] = common_clock + page_swap_time;
    }
}

void migrate(int node_id, uint64_t remote_paddr, uint64_t local_paddr)
{
    // for each migration we need to change two mappings
    int64_t remote_vaddr = get_virtual_address(remote_paddr, node_id);
    int64_t local_vaddr = get_virtual_address(local_paddr, node_id);
    bool pg_tb_update = false;
    // update page-table

    uint64_t ap;
    if (local_vaddr != -1)
    {
        pg_tb_update = update_page_table(_pgd[node_id], remote_paddr, local_vaddr);
        page_table_walk(_pgd[node_id], (local_vaddr << 12), ap);
    }

    pg_tb_update = update_page_table(_pgd[node_id], local_paddr, remote_vaddr);

    L[node_id].update_page_allocation_status(local_paddr);

    page_table_walk(_pgd[node_id], (remote_vaddr << 12), ap);
    update_page_bits(_pgd[node_id], (remote_vaddr << 12), local_paddr, node_id, 0);

    // updating the mapping in reverse map function provider
    if (local_vaddr != -1)
        add_reverse_map(remote_paddr, local_vaddr, node_id);

    add_reverse_map(local_paddr, remote_vaddr, node_id);

    // If per process Page Migration is required then update the procid accordingly
    int procid = 1;
    // invalidating the TLB
    for (int i = 0; i < core_count; i++)
    {
        dtlbs[node_id * core_count + i].InvalidateTLB(remote_vaddr, procid);
        if (local_vaddr != -1)
            dtlbs[node_id * core_count + i].InvalidateTLB(local_vaddr, procid);
        itlbs[node_id * core_count + i].InvalidateTLB(remote_vaddr, procid);
        if (local_vaddr != -1)
            itlbs[node_id * core_count + i].InvalidateTLB(local_vaddr, procid);
        // Invalidating all the cache entries
        il1s[node_id * core_count + i].InvalidateCACHE(remote_paddr, procid);
        dl1s[node_id * core_count + i].InvalidateCACHE(remote_paddr, procid);
        l2s[node_id * core_count + i].InvalidateCACHE(remote_paddr, procid);
        ul3[node_id].InvalidateCACHE(remote_paddr, procid);
        if (local_vaddr != -1)
        {
            il1s[node_id * core_count + i].InvalidateCACHE(local_paddr, procid);
            dl1s[node_id * core_count + i].InvalidateCACHE(local_paddr, procid);
            l2s[node_id * core_count + i].InvalidateCACHE(local_paddr, procid);
            ul3[node_id].InvalidateCACHE(local_paddr, procid);
        }
    }
    double temp = local_plus_remote_page_hotness[node_id][remote_paddr]; // Now since pages have been swapped now i'll just go ahead and swap their hotness as well since now the physical addresses have changed.
    local_plus_remote_page_hotness[node_id][remote_paddr] = local_plus_remote_page_hotness[node_id][local_paddr];
    local_plus_remote_page_hotness[node_id][local_paddr] = temp;
}

bool completing_pending_load_store[num_nodes];
void stall_processor(int node_id)
{
    if (common_clock == background_pg_swap_clock[node_id])
    {
        // cout<<"\nbackground swap complete at cycle "<<background_pg_swap_clock[node_id];
        // page_migration<<"\nbackground swap complete at cycle "<<background_pg_swap_clock[node_id];
        migrate_stall[node_id] = 1;
        completing_pending_load_store[node_id] = 1;
    }
}

bool ok_to_go[num_nodes];

// resume processor and perform the migration.
void resume_processor(int node_id)
{
    if (migrate_stall[node_id] == 1)
    {
        ok_to_go[node_id] = 1;
        for (int i = 0; i < core_count; i++)
        {
            for (int j = 0; j < il1_miss_buffer[node_id][i].size(); j++)
            {
                if (il1_miss_buffer[node_id][i][j].complete_cycle < 1)
                {
                    ok_to_go[node_id] = 0;
                }
            }
            if (dl1_miss_buffer[node_id][i].size() != 0)
                ok_to_go[node_id] = 0;
        }
        if (ok_to_go[node_id] && common_clock > resume_stall_clock[node_id])
        {
            // cout<<"\n resuming and performing migration at cycle"<<common_clock;
            // page_migration<<"\n resuming and performing migration at cycle"<<common_clock;

            completing_pending_load_store[node_id] = 0;
            // from now + 4000 cycles make it 0

            migrate(node_id, remote_hot_paddr[node_id], local_victim_paddr[node_id]);
            if (pages_migrated_in_feedback_cycle[node_id].find(local_victim_paddr[node_id]) != pages_migrated_in_feedback_cycle[node_id].end())
            {
                update_params(node_id, pages_migrated_in_feedback_cycle[node_id][local_victim_paddr[node_id]]);
                pages_migrated_in_feedback_cycle[node_id].erase(local_victim_paddr[node_id]);
            }
            migration_counter[node_id]++;
            pages_migrated_in_feedback_cycle[node_id][local_victim_paddr[node_id]] = 0;
            resume_stall_clock[node_id] = common_clock + TLB_shootdown_cycles;
        }
    }
}
