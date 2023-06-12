from ptlib import *

# zerostate config
CFG0_BOC = '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'

cfg_0 = parse_boc(CFG0_BOC)
cfg_0 = parse_boc(cfg_0.begin_parse().fetch_ref().to_boc())

zerostate_accounts = [
    '0' * 64,
    '3' * 64,
    '5' * 64,
    '6' * 64,
    '34517C7BDF5187C55AF4F8B61FDC321588C7AB768DEE24B006DF29106458D7CF',
    'FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEF'
]


def fhex(x):
    return hex(x).upper()[2:].zfill(64)


def covert_libs_cell_to_dict(libs_info):
    block_libs = VmDict(256)

    for lib in libs_info:
        c = parse_boc(lib)

        hash_key = c.get_hash()
        c = begin_cell().store_ref(c).end_cell().begin_parse()

        block_libs.set(int(hash_key, 16), c)

    return block_libs.get_cell()


def covert_account_libs_cell_to_dict(libs_info):
    # library:(HashmapE 256 SimpleLib);
    # simple_lib$_ public:Bool root:^Cell = SimpleLib;

    block_libs = VmDict(256)
    # account_state_state_init_libs_public
    # account_state_state_init_libs_cell

    for lib, public in zip(libs_info['account_state_state_init_libs_cell'],
                           libs_info['account_state_state_init_libs_public']):
        c = parse_boc(lib)

        hash_key = c.get_hash()
        c = begin_cell().store_uint(int(public), 1) \
            .store_ref(c).end_cell().begin_parse()

        block_libs.set(int(hash_key, 16), c)

    return block_libs.get_cell()


# extra_currencies$_ dict:(HashmapE 32 (VarUInteger 32))
#                  = ExtraCurrencyCollection;

# currencies$_ grams:Grams other:ExtraCurrencyCollection
#            = CurrencyCollection;

currency_collection = lambda grams: begin_cell().store_var_uint(grams, 16).store_uint(0, 1)


# tick_tock$_ tick:Bool tock:Bool = TickTock;

# _ split_depth:(Maybe (## 5)) special:(Maybe TickTock)
#   code:(Maybe ^Cell) data:(Maybe ^Cell)
#   library:(Maybe ^Cell) = StateInit;

# account_uninit$00 = AccountState;
# account_active$1 _:StateInit = AccountState;
# account_frozen$01 state_hash:bits256 = AccountState;

def get_account_state(transaction):
    if transaction['account_state_type'] == 'active':
        c = begin_cell().store_ones(1)

        if transaction['account_state_state_init_split_depth'] is not None:
            c = c.store_ones(1) \
                .store_uint(transaction['account_state_state_init_split_depth'], 5)
        else:
            c = c.store_zeroes(1)

        if transaction['account_state_state_init_special_tick'] is not None or \
                transaction['account_state_state_init_special_tock'] is not None:
            c = c.store_ones(1) \
                .store_uint(transaction['account_state_state_init_special_tick'], 1) \
                .store_uint(transaction['account_state_state_init_special_tock'], 1)
        else:
            c = c.store_zeroes(1)

        if transaction['account_state_state_init_code'] is not None:
            c = c.store_ones(1).store_ref(parse_boc(transaction['account_state_state_init_code']))
        else:
            c = c.store_zeroes(1)

        if transaction['account_state_state_init_data'] is not None:
            c = c.store_ones(1).store_ref(parse_boc(transaction['account_state_state_init_data']))
        else:
            c = c.store_zeroes(1)

        if len(transaction['account_state_state_init_libs_cell']):
            c = c.store_ones(1).store_ref(covert_account_libs_cell_to_dict(transaction))
        else:
            c = c.store_zeroes(1)

        return c
    elif transaction['account_state_type'] == 'frozen':
        c = begin_cell().store_bitstring("01") \
            .store_uint(int(transaction['account_state_state_hash'], 16), 256)
    else:
        return begin_cell().store_zeroes(2)


# storage_used$_ cells:(VarUInteger 7) bits:(VarUInteger 7)
#   public_cells:(VarUInteger 7) = StorageUsed;

# storage_info$_ used:StorageUsed last_paid:uint32
#               due_payment:(Maybe Grams) = StorageInfo;

def get_storage_info(transaction):
    c = begin_cell().store_var_uint(transaction['account_storage_stat_used_cells'], 7) \
        .store_var_uint(transaction['account_storage_stat_used_bits'], 7) \
        .store_var_uint(transaction['account_storage_stat_used_public_cells'], 7) \
        .store_uint(transaction['account_storage_stat_last_paid'], 32)

    if transaction['account_storage_stat_due_payment'] and int(transaction['account_storage_stat_due_payment']) > 0:
        c = c.store_ones(1).store_grams(transaction['account_storage_stat_due_payment'])
    else:
        c = c.store_zeroes(1)

    return c


def get_empty_shard_account():
    return begin_cell() \
        .store_ref(begin_cell().store_uint(0, 1).end_cell()).store_uint(0, 256).store_uint(0, 64).end_cell()


def pack_account_state(transaction):
    # account_storage$_ last_trans_lt:uint64
    #     balance:CurrencyCollection state:AccountState
    #   = AccountStorage;

    account_storage = begin_cell() \
        .store_uint(transaction['account_storage_last_trans_lt'], 64) \
        .store_builder(currency_collection(transaction['account_storage_balance_grams'])) \
        .store_builder(get_account_state(transaction))

    # account_none$0 = Account;
    # account$1 addr:MsgAddressInt storage_stat:StorageInfo
    #           storage:AccountStorage = Account;

    # account_descr$_ account:^Account last_trans_hash:bits256
    #   last_trans_lt:uint64 = ShardAccount;

    c1 = begin_cell() \
        .store_ones(1) \
        .store_address(f"{transaction['workchain']}:{transaction['address']}") \
        .store_builder(get_storage_info(transaction)) \
        .store_builder(account_storage) \
        .end_cell()

    c2 = begin_cell() \
        .store_ref(c1) \
        .store_uint(int(transaction['account_last_trans_hash'], 16), 256) \
        .store_uint(transaction['account_last_trans_lt'], 64) \
        .end_cell()

    return c2
