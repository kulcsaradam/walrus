// #include <cstddef>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <cstdint>

enum status_flag : uint8_t {
    free_32,
    free_64,
    free_128,
    used_32,
    used_64,
    used_128,
};

struct stack_element {
    stack_element* prev;
    stack_element* next;
    size_t position;
    status_flag flag;
};

// info that we already have
enum types {
    I32,
    I64,
    V128
};

struct range {
    types type;
    size_t start;
    size_t end;
    stack_element* assigned_space;

    range(types type, size_t start, size_t end)
        : type(type)
        , start(start)
        , end(end)
        , assigned_space(nullptr)
    {
    }
};

// helpers

void error(std::string err_msg)
{
    std::cout << err_msg + "\n";
    std::abort();
}

void print_stack_elems(std::vector<stack_element> stack_elements)
{
    for (auto& elem : stack_elements) {
        printf("pos: %lu ", elem.position);
        switch (elem.flag) {
        case free_32: {
            printf("type: 32\n");
            break;
        }
        case free_64: {
            printf("type: 64\n");
            break;
        }
        case free_128: {
            printf("type: 128\n");
            break;
        }
        default: {
            printf("Error\n");
            std::abort();
            break;
        }
        }
    }
}

int main(int argc, char* argv[])
{
    // fix infos before we start the algorithm
    size_t bytecode_end = 10;
    size_t free_space = 0;
    std::vector<range> ranges = {
        range(I32, 0, 1),
        range(I64, 0, 3),
        range(I32, 0, 2),
        range(I64, 2, 3),
        range(V128, 3, 5),
    };

    // end of fix infos

    size_t max = 0;
    for (size_t i = 0; i < bytecode_end; i++) {
        for (auto& elem : ranges) {
            if (elem.start == i) {
                switch (elem.type) {
                case types::I32: {
                    free_space += 4;
                    break;
                }
                case types::I64: {
                    free_space += 8;
                    break;
                }
                case types::V128: {
                    free_space += 16;
                    break;
                }
                default: {
                    error("Type does not exists");
                    break;
                }
                }
            }

            if (max < free_space) {
                max = free_space;
            }

            if (elem.end == i) {
                switch (elem.type) {
                case types::I32: {
                    free_space -= 4;
                    break;
                }
                case types::I64: {
                    free_space -= 8;
                    break;
                }
                case types::V128: {
                    free_space -= 16;
                    break;
                }
                default: {
                    error("Type does not exists");
                    break;
                }
                }
            }
        }
    }
    size_t required_size = max;
    free_space = max;

    size_t offset = 0;
    std::vector<stack_element*> free_slots_128 = {};
    std::vector<stack_element*> free_slots_64 = {};
    std::vector<stack_element*> free_slots_32 = {};
    stack_element* slotsHead = nullptr;
    stack_element* slotsTail = nullptr;

    stack_element* unused_var_elem = new stack_element({ nullptr, nullptr, offset, status_flag::used_32 });

    for (auto range : ranges) {
        if (range.start == range.end) {
        }
    }

    while (free_space > 0) {
        if (free_space >= 16) {
            if (slotsHead == nullptr) {
                slotsHead = new stack_element({ (stack_element*)-1, nullptr, offset, free_128 });
                slotsTail = slotsHead;
            } else {
                stack_element* new_elem = new stack_element({ slotsTail, nullptr, offset, free_128 });
                slotsTail->next = new_elem;
                slotsTail = new_elem;
            }

            free_slots_128.push_back(slotsTail);
            offset += 16;
            free_space -= 16;
        } else if (free_space >= 8) {
            if (slotsHead == nullptr) {
                slotsHead = new stack_element({ (stack_element*)-1, nullptr, offset, free_64 });
                slotsTail = slotsHead;
            } else {
                stack_element* new_elem = new stack_element({ slotsTail, nullptr, offset, free_64 });
                slotsTail->next = new_elem;
                slotsTail = new_elem;
            }

            free_slots_64.push_back(slotsTail);
            offset += 8;
            free_space -= 8;
        } else if (free_space >= 4) {
            if (slotsHead == nullptr) {
                slotsHead = new stack_element({ (stack_element*)-1, nullptr, offset, free_32 });
                slotsTail = slotsHead;
            } else {
                stack_element* new_elem = new stack_element({ slotsTail, nullptr, offset, free_32 });
                slotsTail->next = new_elem;
                slotsTail = new_elem;
            }

            free_slots_32.push_back(slotsTail);
            offset += 4;
            free_space -= 4;
        } else {
            error("Error occured in space allocation");
        }
    }

    for (size_t i = 0; i < bytecode_end; i++) {
        for (auto& range : ranges) {
            if (range.end == range.start) {
                continue;
            }

            if (range.end == i) {
                if (range.assigned_space == nullptr) {
                    break;
                    // error("Nullptr was assigned");
                }

                switch (range.type) {
                case types::I32: {
                    stack_element* current = range.assigned_space;
                    if (current->next != nullptr && current->next->flag == status_flag::free_32 && current->next->position + 4 % 8 == 0) {
                        stack_element* new_elem = new stack_element({ current->prev, current->next->next, current->position, status_flag::free_64 });
                        if (current->prev != (stack_element*)-1) {
                            current->prev->next = new_elem;
                        }
                        if (current->next->next != nullptr) {
                            current->next->next->prev = new_elem;
                        }
                        free_slots_64.push_back(new_elem);

                        delete current->next;
                        delete current;
                    } else if (current->prev != (stack_element*)-1 && current->prev->flag == status_flag::free_32 && current->prev->position % 8 == 0) {
                        stack_element* new_elem = new stack_element({ current->prev->prev, current->next, current->prev->position, status_flag::free_64 });

                        if (current->next != nullptr) {
                            current->next->prev = new_elem;
                        }
                        if (current->prev->prev != (stack_element*)-1) {
                            current->prev->prev->next = new_elem;
                        }
                        free_slots_64.push_back(new_elem);

                        delete current->prev;
                        delete current;
                    } else {
                        current->flag = status_flag::free_32;
                        free_slots_32.push_back(current);
                    }
                    break;
                }
                case types::I64: {
                    stack_element* current = range.assigned_space;
                    if (current->next != nullptr && current->next->flag == status_flag::free_64 && current->next->position + 8 % 16 == 0) {
                        stack_element* new_elem = new stack_element({ current->prev, current->next->next, current->position, status_flag::free_128 });
                        if (current->prev != (stack_element*)-1) {
                            current->prev->next = new_elem;
                        }
                        if (current->next->next != nullptr) {
                            current->next->next->prev = new_elem;
                        }
                        free_slots_128.push_back(new_elem);

                        delete current->next;
                        delete current;
                    } else if (current->prev != (stack_element*)-1 && current->prev->flag == status_flag::free_64 && current->prev->position % 16 == 0) {
                        stack_element* new_elem = new stack_element({ current->prev->prev, current->next, current->prev->position, status_flag::free_128 });
                        if (current->next != nullptr) {
                            current->next->prev = new_elem;
                        }
                        if (current->prev->prev != (stack_element*)-1) {
                            current->prev->prev->next = new_elem;
                        }
                        free_slots_128.push_back(new_elem);

                        delete current->prev;
                        delete current;
                    } else {
                        current->flag = status_flag::free_64;
                        free_slots_64.push_back(current);
                    }
                    break;
                }
                case types::V128: {
                    range.assigned_space->flag = status_flag::free_128;
                    free_slots_128.push_back(range.assigned_space);
                    break;
                }
                default: {
                    error("Invalid type encountered");
                }
                }
            }

            if (range.start == i) {
                // "allocate" the needed space
                if (range.type == types::I32) {
                    if (!free_slots_32.empty()) {
                        stack_element* slot = free_slots_32.back();
                        free_slots_32.pop_back();
                        range.assigned_space = slot;
                        slot->flag = status_flag::used_32;
                    } else if (!free_slots_64.empty()) {
                        stack_element* current = free_slots_64.back();
                        free_slots_64.pop_back();
                        stack_element* slot_next = current->next;

                        stack_element* new_used = new stack_element({ current->prev, nullptr, current->position, status_flag::used_32 });
                        stack_element* new_unused = new stack_element({ new_used, slot_next, current->position + 4, status_flag::free_32 });
                        new_used->next = new_unused;

                        delete current;
                        free_slots_32.push_back(new_unused);
                        range.assigned_space = new_used;
                    } else if (!free_slots_128.empty()) {
                        stack_element* current = free_slots_128.back();
                        free_slots_128.pop_back();
                        stack_element* slot_next = current->next;

                        stack_element* new_used = new stack_element({ current->prev, nullptr, current->position, status_flag::used_32 });
                        stack_element* new_unused32 = new stack_element({ new_used, nullptr, current->position + 4, status_flag::free_32 });
                        new_used->next = new_unused32;
                        stack_element* new_unused64 = new stack_element({ new_unused32, slot_next, current->position + 8, status_flag::free_64 });
                        new_unused32->next = new_unused64;

                        delete current;
                        free_slots_32.push_back(new_unused32);
                        free_slots_64.push_back(new_unused64);
                        range.assigned_space = new_used;
                    } else {
                        error("No available space to allocate variables");
                    }

                } else if (range.type == types::I64) {
                    if (!free_slots_64.empty()) {
                        stack_element* slot = free_slots_64.back();
                        free_slots_64.pop_back();

                        range.assigned_space = slot;
                        slot->flag = status_flag::used_64;
                    } else if (!free_slots_128.empty()) {
                        stack_element* current = free_slots_128.back();
                        free_slots_128.pop_back();
                        stack_element* slot_next = current->next;

                        stack_element* new_used = new stack_element({ current->prev, nullptr, current->position, status_flag::used_64 });
                        stack_element* new_unused = new stack_element({ new_used, slot_next, current->position + 8, status_flag::free_64 });
                        new_used->next = new_unused;

                        delete current;
                        free_slots_64.push_back(new_unused);
                        range.assigned_space = new_used;
                    } else {
                        error("No available space to allocate variables");
                    }
                } else if (range.type == types::V128) {
                    if (!free_slots_128.empty()) {
                        stack_element* slot = free_slots_128.back();
                        free_slots_128.pop_back();

                        range.assigned_space = slot;
                        slot->flag = status_flag::used_128;
                    }
                } else {
                    error("Invalid type in allocation");
                }
            }
        }
    }
    int a = 42;

    return 0;
}
