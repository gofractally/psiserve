wit_macro::wit_world! {
    package = "test:inventory@1.0.0";

    // ── Record types ────────────────────────────────────────────────────
    // Must match the C++ structs in wit_compare_test.cpp

    pub struct Item {
        pub id: u64,
        pub name: String,
        pub category: String,
        pub price_cents: u32,
        pub in_stock: bool,
        pub tags: Vec<String>,
        pub weight_grams: Option<u32>,
    }

    pub struct StockResult {
        pub item_id: u32,
        pub old_quantity: u32,
        pub new_quantity: u32,
    }

    pub struct SearchQuery {
        pub text: String,
        pub max_results: u32,
        pub min_price: Option<u32>,
        pub max_price: Option<u32>,
        pub categories: Vec<String>,
    }

    pub struct SearchResponse {
        pub items: Vec<Item>,
        pub total_count: u64,
        pub has_more: bool,
    }

    pub struct BulkResult {
        pub inserted: u32,
        pub failed: u32,
        pub errors: Vec<String>,
    }

    // ── Interface ───────────────────────────────────────────────────────

    pub trait InventoryApi {
        fn get_item(&self, item_id: u32) -> Option<Item>;
        fn list_items(&self, category: String) -> Vec<Item>;
        fn add_item(&self, item: Item) -> u64;
        fn update_stock(&self, item_id: u32, delta: i32) -> StockResult;
        fn search(&self, query: SearchQuery) -> SearchResponse;
        fn bulk_import(&self, items: Vec<Item>) -> BulkResult;
        fn ping(&self);
    }
}
