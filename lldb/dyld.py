import lldb
import lldb.formatters.Logger

# dyld formatters

class Vector_SynthProvider:
    def __init__(self, valobj, dict):
        logger = lldb.formatters.Logger.Logger()
        self.valobj = valobj
        self.count = 0

    def num_children(self):
        logger = lldb.formatters.Logger.Logger()
        try:
            return self.count
        except:
            return 0

    def get_child_index(self, name):
        logger = lldb.formatters.Logger.Logger()
        try:
            return int(name.lstrip('[').rstrip(']'))
        except:
            return -1

    def get_child_at_index(self, index):
        logger = lldb.formatters.Logger.Logger()
        if index < 0:
            return None
        if index >= self.num_children():
            return None
        try:
            offset = index * self.data_size
            return self.data.CreateChildAtOffset(
                '[' + str(index) + ']', offset, self.data_type)
        except:
            return None

    def update(self):
        logger = lldb.formatters.Logger.Logger()
        try:
            self.count = self.valobj.GetChildMemberWithName('_size').GetValueAsUnsigned(0)
            self.data = self.valobj.GetChildMemberWithName('_buffer')
            # the purpose of this field is unclear, but it is the only field whose type is clearly T* for a vector<T>
            # if this ends up not being correct, we can use the APIs to get at
            # template arguments
            data_type_finder = self.data
            self.data_type = data_type_finder.GetType().GetPointeeType()
            self.data_size = self.data_type.GetByteSize()
        except:
            pass

    def has_children(self):
        return True

class OrderedSet_SynthProvider:
    def __init__(self, valobj, dict):
        logger = lldb.formatters.Logger.Logger()
        self.valobj = valobj
        self.count = 0

    def num_children(self):
        logger = lldb.formatters.Logger.Logger()
        try:
            return self.count
        except:
            return 0

    def get_child_index(self, name):
        logger = lldb.formatters.Logger.Logger()
        try:
            return int(name.lstrip('[').rstrip(']'))
        except:
            return -1

    def get_child_at_index(self, index):
        logger = lldb.formatters.Logger.Logger()
        if index < 0:
            return None
        if index >= self.num_children():
            return None
        try:
            offset = index * self.data_size
            return self.data.CreateChildAtOffset(
                '[' + str(index) + ']', offset, self.data_type)
        except:
            return None

    def update(self):
        logger = lldb.formatters.Logger.Logger()
        try:
            self.count = self.valobj.GetChildMemberWithName('_size').GetValueAsUnsigned(0)
            self.data = self.valobj.GetChildMemberWithName('_buffer')
            # the purpose of this field is unclear, but it is the only field whose type is clearly T* for a vector<T>
            # if this ends up not being correct, we can use the APIs to get at
            # template arguments
            data_type_finder = self.data
            self.data_type = data_type_finder.GetType().GetPointeeType()
            self.data_size = self.data_type.GetByteSize()
        except:
            pass

    def has_children(self):
        return True

def __lldb_init_module(debugger, dict):
    lldb.formatters.Logger._lldb_formatters_debug_level = 2
    lldb.formatters.Logger._lldb_formatters_debug_filename = "/tmp/lldb.log"
    logger = lldb.formatters.Logger.Logger()
    logger >> "Loading"
    debugger.HandleCommand(
        'type synthetic add -l dyld.Vector_SynthProvider -x "^lsl::Vector<.+>$" -w dyld')
    debugger.HandleCommand(
        'type summary add --summary-string size=${var._size} -e -x "^lsl::Vector<.+>$" -w dyld')
#    debugger.HandleCommand(
#        'type synthetic add -l dyld.OrderedSet_SynthProvider -x "^dyld4::OrderedSet<.+>$" -w dyld')
#    debugger.HandleCommand(
#        'type summary add --summary-string size=${var._size} -e -x "^dyld4::OrderedSet<.+>$" -w dyld')
    debugger.HandleCommand("type category enable dyld")

