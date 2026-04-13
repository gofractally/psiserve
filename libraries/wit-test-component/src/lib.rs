use wit_macro::wit_interface;

#[wit_interface(package = "myapp:balance@1.0.0")]
pub trait BalanceApi {
    fn get_balance(&self, account_id: u32) -> u64;
    fn transfer(&self, sender: u32, receiver: u32, amount: u64) -> Result<(), String>;
}

#[wit_interface(package = "myapp:echo@1.0.0")]
pub trait EchoApi {
    fn echo(&self, message: String) -> String;
    fn echo_bytes(&self, data: Vec<u8>) -> Vec<u8>;
    fn ping(&self);
}
