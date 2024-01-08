// #include <cstddef>
#include <algorithm>
#include <iterator>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <cstdint>


enum status_flag {
    free_32,
    free_64,
    free_128,
    used
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
    bool needs_init;

    range(types type, size_t start, size_t end, bool needs_init)
        : type(type)
        , start(start)
        , end(end)
        , assigned_space(nullptr)
        , needs_init(needs_init)
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
        range(I64, 3, 6, false),
        range(I32, 0, 4, true),
        range(I32, 2, 7, false),
        range(V128, 8, 9, false),
        range(I32, 6, 6, true),
        range(I64, 0, 10, false),
        range(I64, 0, 10, false),
    };

    // end of fix infos

    size_t max = 0;
    for (size_t i = 0; i < bytecode_end; i++) {
        for (auto& elem : ranges) {
            if (elem.start == i) {
                switch (elem.type) {
                case types::I32: {
                    free_space += 32;
                    break;
                }
                case types::I64: {
                    free_space += 64;
                    break;
                }
                case types::V128: {
                    free_space += 128;
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
                    free_space -= 32;
                    break;
                }
                case types::I64: {
                    free_space -= 64;
                    break;
                }
                case types::V128: {
                    free_space -= 128;
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

    // std::vector<stack_element*> slots_128 = {};
    // std::vector<stack_element*> slots_64 = {};
    // std::vector<stack_element*> slots_32 = {};

    stack_element* slots128Head = nullptr;
    stack_element* slots128Tail = nullptr;

    stack_element* slots64Head = nullptr;
    stack_element* slots64Tail = nullptr;

    stack_element* slots32Head = nullptr;
    stack_element* slots32Tail = nullptr;

    while (free_space > 0) {
        if (free_space >= 128) {
            // stack_elements.push_back({ offset, free_128 });
            // slots_128.push_back(new stack_element({ offset, free_128 }));

            if (slots128Head == nullptr) {
                slots128Head = new stack_element({ (stack_element*)-1, nullptr, offset, free_128 });
                slots128Tail = slots128Head;
            } else {
                stack_element* new_elem = new stack_element({ slots128Tail, nullptr, offset, free_128 });
                slots128Tail->next = new_elem;
                slots128Tail = new_elem;
            }

            offset += 128;
            free_space -= 128;
        } else if (free_space >= 64) {
            // slots_64.push_back(new stack_element({ offset, free_64 }));

            if (slots64Head == nullptr) {
                slots64Head = new stack_element({ (stack_element*)-1, nullptr, offset, free_64 });
                slots64Tail = slots64Head;
            } else {
                stack_element* new_elem = new stack_element({ slots64Tail, nullptr, offset, free_64 });
                slots64Tail->next = new_elem;
                slots64Tail = new_elem;
            }

            offset += 64;
            free_space -= 64;
        } else if (free_space >= 32) {
            // slots_32.push_back(new stack_element({ offset, free_32 }));

            if (slots32Head == nullptr) {
                slots32Head = new stack_element({ (stack_element*)-1, nullptr, offset, free_32 });
                slots32Tail = slots32Head;
            } else {
                stack_element* new_elem = new stack_element({ slots32Tail, nullptr, offset, free_32 });
                slots32Tail->next = new_elem;
                slots32Tail = new_elem;
            }

            offset += 32;
            free_space -= 32;
        } else {
            error("Error occured in space allocation");
        }
    }

    for (size_t i = 0; i < bytecode_end; i++) {
        for (auto& range : ranges) {
            if (range.start == i) {
                // "allocate" the needed space
                if (range.type == types::I32) {
                    if (!slots_32.empty()) {
                        stack_element* slot = slots_32.back();
                        slots_32.pop_back();

                        range.assigned_space = slot;
                        slot->flag = status_flag::used;
                    } else if (!slots_64.empty()) {
                        stack_element* slot = slots_64.back();
                        slots_64.pop_back();

                        // stack_element* useable_32 = new stack_element({ slot->position, status_flag::used });
                        // stack_element* unused_32 = new stack_element({ slot->position + 32, status_flag::free_32 });

                        // delete slot;
                        range.assigned_space = useable_32;
                        slots_32.push_back(unused_32);
                    } else if (!slots_128.empty()) {
                        stack_element* slot = slots_128.back();
                        slots_128.pop_back();

                        stack_element* useable_32 = new stack_element({ slot->position, status_flag::used });
                        stack_element* unused_32 = new stack_element({ slot->position + 32, status_flag::free_32 });
                        stack_element* unused_64 = new stack_element({ slot->position + 64, status_flag::free_64 });

                        // delete slot;
                        range.assigned_space = useable_32;
                        slots_32.push_back(unused_32);
                        slots_64.push_back(unused_64);
                    } else {
                        error("No available space to allocate variables");
                    }

                } else if (range.type == types::I64) {
                    if (!slots_64.empty()) {
                        stack_element* slot = slots_64.back();
                        slots_64.pop_back();

                        range.assigned_space = slot;
                        slot->flag = status_flag::used;
                    } else if (!slots_128.empty()) {
                        stack_element* slot = slots_128.back();
                        slots_128.pop_back();

                        stack_element* useable_64 = new stack_element({ slot->position, status_flag::used });
                        stack_element* unused_64 = new stack_element({ slot->position + 64, status_flag::free_64 });

                        // delete slot;
                        range.assigned_space = useable_64;
                        slots_64.push_back(unused_64);

                    } else if (slots_32.size() >= 2) {
                        size_t pos = SIZE_MAX;
                        for (auto it = slots_32.begin(); it != slots_32.end(); it++) {
                            auto temp = std::find_if(slots_32.begin(), slots_32.end(), [it](stack_element* elem) {
                                if (elem->position + 32 == (*it)->position || elem->position - 32 == (*it)->position) {
                                    return true;
                                }

                                return false;
                            });

                            if (temp != slots_32.end()) {
                                if ((*it)->position < (*temp)->position) {
                                    pos = (*it)->position;
                                } else {
                                    pos = (*temp)->position;
                                }

                                slots_32.erase(it);
                                slots_32.erase(temp);
                                // delete *it;
                                // delete *temp;
                                break;
                            }
                        }

                        if (pos == SIZE_MAX) {
                            error("Something went wrong in I64 allocation at I32 consolidation");
                        }

                        stack_element* useable_64 = new stack_element({ pos, status_flag::used });
                        range.assigned_space = useable_64;
                    } else {
                        error("No available space to allocate variables");
                    }
                } else if (range.type == types::V128) {
                    if (!slots_128.empty()) {
                        stack_element* slot = slots_64.back();
                        slots_64.pop_back();

                        stack_element* useable_32 = new stack_element({ slot->position, slot->flag });
                        stack_element* unused_32 = new stack_element({ slot->position + 32, slot->flag });

                        // delete slot;
                        range.assigned_space = useable_32;
                        slots_32.push_back(unused_32);
                    } else if (slots_64.size() >= 2) {
                        size_t pos = SIZE_MAX;
                        for (auto it = slots_64.begin(); it != slots_64.end(); it++) {
                            auto temp = std::find_if(slots_64.begin(), slots_64.end(), [it](stack_element* elem) {
                                if (elem->position + 64 == (*it)->position || elem->position - 64 == (*it)->position) {
                                    return true;
                                }

                                return false;
                            });

                            if (temp != slots_64.end()) {
                                if ((*it)->position < (*temp)->position) {
                                    pos = (*it)->position;
                                } else {
                                    pos = (*temp)->position;
                                }

                                slots_64.erase(it);
                                slots_64.erase(temp);
                                // delete *it;
                                // delete *temp;
                                break;
                            }
                        }

                        if (pos == SIZE_MAX) {
                            error("Something went wrong in I64 allocation at I32 consolidation");
                        }

                        stack_element* useable_128 = new stack_element({ pos, status_flag::used });
                        range.assigned_space = useable_128;
                    } else if (slots_32.size() >= 4) {
                        std::vector<stack_element*> elements = {};

                        auto it = slots_32.begin();
                        while (it != slots_32.end()) {
                            elements.push_back(*it);


                            if (elements.size() > 3) {
                                break;
                            }
                            auto temp = std::find_if(slots_32.begin(), slots_32.end(), [it](stack_element* elem) {
                                if ((*it)->position + 32 == elem->position) {
                                    return true;
                                }

                                return false;
                            });

                            if (temp == slots_32.end()) {
                                elements.clear();
                                it++;
                                continue;
                            }

                            it = temp;
                        }

                        size_t pos = elements[0]->position;
                        for (auto& elem : elements) {
                            if (elem->position < pos) {
                                pos = elem->position;
                            }
                        }

                        stack_element* useable_128 = new stack_element({ pos, status_flag::used });
                        for (auto& elem : elements) {
                            // delete elem;
                        }
                        elements.clear();

                        range.assigned_space = useable_128;
                    } else if (!slots_64.empty() && !slots_32.empty()) {
                        std::vector<stack_element*> elements = {};
                        std::vector<stack_element*> data = {};
                        data.insert(data.end(), slots_32.begin(), slots_32.end());
                        data.insert(data.end(), slots_64.begin(), slots_64.end());
                        auto it = data.begin();
                        while (it != data.end()) {
                            elements.push_back(*it);


                            if (elements.size() > 2) {
                                break;
                            }
                            auto temp = std::find_if(data.begin(), data.end(), [it](stack_element* elem) {
                                if ((*it)->position + 32 == elem->position || (*it)->position + 64 == elem->position) {
                                    return true;
                                }

                                return false;
                            });

                            if (temp == data.end()) {
                                elements.clear();
                                it++;
                                continue;
                            }

                            it = temp;
                        }

                        size_t pos = elements[0]->position;
                        for (auto& elem : elements) {
                            if (elem->position < pos) {
                                pos = elem->position;
                            }
                        }

                        stack_element* useable_128 = new stack_element({ pos, status_flag::used });
                        for (auto& elem : elements) {
                            // delete elem;
                        }
                        elements.clear();

                        range.assigned_space = useable_128;
                    } else {
                        error("No available space to allocate variables");
                    }
                } else {
                    error("Invalid type in allocation");
                }
            }

            if (range.end == i) {
                if (range.assigned_space == nullptr) {
                    error("Nullptr was assigned");
                }

                switch (range.type) {
                case types::I32: {
                    range.assigned_space->flag = status_flag::free_32;
                    slots_32.push_back(range.assigned_space);
                    break;
                }
                case types::I64: {
                    range.assigned_space->flag = status_flag::free_64;
                    slots_64.push_back(range.assigned_space);
                    break;
                }
                case types::V128: {
                    range.assigned_space->flag = status_flag::free_128;
                    slots_128.push_back(range.assigned_space);
                    break;
                }
                default: {
                    error("Invalid type encountered");
                }
                }

                // range.assigned_space = nullptr;
            }
        }
    }


    return 0;
}
