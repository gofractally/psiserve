In an ideal world, this is how I would like to code my host services:


class Database {
    
};

class Store {
     public:
         Store( std::path dir ):_my_db(dir){}

         shared_ptr<Database> getDatabase( string name ) {
            auto root = _my_db.get_root( _top_root_names[name] ); /// notional...
         }

     private:
         psitri::database _my_db;
         write_session    _write_ses;
         map<string, int> _top_root_names;
};



int main() {

    Store my_store( "db_dir" );

    auto pz = load_pzam( "website.pzam" ); // notional
    pz.link( my_store );


}
