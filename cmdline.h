/*
Copyright (c) 2009, Hideyuki Tanaka
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY <copyright holder> ''AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <copyright holder> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once

#include <iostream>
#include <sstream>
#include <vector>
#include <map>
#include <string>
#include <stdexcept>
#include <typeinfo>
#include <cstring>
#include <cxxabi.h>
#include <malloc.h>

namespace cmdline{

namespace detail{

template <typename Target, typename Source, bool Same>
class lexical_cast_t{
public:
  static Target cast(const Source &arg){
    Target ret;
    std::stringstream ss;
    if (!(ss<<arg && ss>>ret && ss.eof()))
      throw std::bad_cast();
    
    return ret;
  }
};

template <typename Target, typename Source>
class lexical_cast_t<Target, Source, true>{
public:
  static Target cast(const Source &arg){
    return arg;
  }  
};

template <typename Source>
class lexical_cast_t<std::string, Source, false>{
public:
  static std::string cast(const Source &arg){
    std::ostringstream ss;
    ss<<arg;
    return ss.str();
  }
};

template <typename Target>
class lexical_cast_t<Target, std::string, false>{
public:
  static Target cast(const std::string &arg){
    Target ret;
    std::istringstream ss(arg);
    if (!(ss>>ret && ss.eof()))
      throw std::bad_cast();
    return ret;
  }
};

template <typename T1, typename T2>
struct is_same {
  static const bool value = false;
};

template <typename T>
struct is_same<T, T>{
  static const bool value = true;
};

template<typename Target, typename Source>
Target lexical_cast(const Source &arg)
{
  return lexical_cast_t<Target, Source, detail::is_same<Target, Source>::value>::cast(arg);
}


template <class T>
struct lexical_caster{
  T operator()(const std::string &str){
    return lexical_cast<T>(str);
  }
};

static inline std::string demangle(const std::string &name)
{
  int status=0;
  char *p=abi::__cxa_demangle(name.c_str(), 0, 0, &status);
  std::string ret(p);
  free(p);
  return ret;
}

template <class T>
std::string readable_typename()
{
  return demangle(typeid(T).name());
}

template <>
std::string readable_typename<std::string>()
{
  return "string";
}

} // detail

//-----

class cmdline_error : public std::exception {
public:
  cmdline_error(const std::string &msg): msg(msg){}
  ~cmdline_error() throw() {}
  const char *what() const throw() { return msg.c_str(); }
private:
  std::string msg;
};

template <class T>
struct range_reader{
  range_reader(const T &low, const T &high): low(low), high(high) {}
  T operator()(const std::string &s){
    T ret=cmdline::detail::lexical_cast<T>(s);
    if (!(ret>=low && ret<=high)) throw cmdline::cmdline_error("range_error");
    return ret;
  }
private:
  T low, high;
};

template <class T>
range_reader<T> range(const T &low, const T &high)
{
  return range_reader<T>(low, high);
}

class parser{
public:
  parser(){
  }
  ~parser(){
    for (std::map<std::string, option_base*>::iterator p=options.begin();
	 p!=options.end(); p++)
      delete p->second;
  }

  void add(const std::string &name,
	   char short_name=0,
	   const std::string &desc=""){
    if (options.count(name)) throw cmdline_error("multiple definition: "+name);
    options[name]=new option_without_value(name, short_name, desc);
    ordered.push_back(options[name]);
  }

  template <class T>
  void add(const std::string &name,
	   char short_name=0,
	   const std::string &desc="",
	   bool need=true,
	   const T def=T()){
    typedef detail::lexical_caster<T> F;
    if (options.count(name)) throw cmdline_error("multiple definition: "+name);
    options[name]=new option_with_value_with_reader<T, F>(name, short_name, need, def, desc, F());
    ordered.push_back(options[name]);
  }

  template <class T, class F>
  void add(const std::string &name,
	   char short_name=0,
	   const std::string &desc="",
	   bool need=true,
	   const T def=T(),
	   F reader=F()){
    if (options.count(name)) throw cmdline_error("multiple definition: "+name);
    options[name]=new option_with_value_with_reader<T, F>(name, short_name, need, def, desc, reader);
    ordered.push_back(options[name]);
  }

  void footer(const std::string &f){
    ftr=f;
  }

  bool exist(const std::string &name){
    if (options.count(name)==0) throw cmdline_error("there is no flag: --"+name);
    return options[name]->has_set();
  }

  template <class T>
  const T &get(const std::string &name) const {
    if (options.count(name)==0) throw cmdline_error("there is no flag: --"+name);
    const option_with_value<T> *p=dynamic_cast<const option_with_value<T>*>(options.find(name)->second);
    if (p==NULL) throw cmdline_error("type mismatch flag '"+name+"'");
    return p->get();
  }

  const std::vector<std::string> &rest() const {
    return others;
  }

  bool parse(int argc, char *argv[]){
    errors.clear();
    others.clear();

    if (argc<1){
      errors.push_back("argument number must be longer than 0");
      return false;
    }
    prog_name=argv[0];

    std::map<char, std::string> lookup;
    for (std::map<std::string, option_base*>::iterator p=options.begin();
	 p!=options.end(); p++){
      if (p->first.length()==0) continue;
      char initial=p->second->short_name();
      if (initial){
	if (lookup.count(initial)>0){
	  lookup[initial]="";
	  errors.push_back(std::string("short option '")+initial+"' is ambiguous");
	  return false;
	}
	else lookup[initial]=p->first;
      }
    }

    for (int i=1; i<argc; i++){
      if (strncmp(argv[i], "--", 2)==0){
	char *p=strchr(argv[i]+2, '=');
	if (p){
	  std::string name(argv[i]+2, p);
	  std::string val(p+1);
	  set_option(name, val);
	}
	else{
	  std::string name(argv[i]+2);
	  set_option(name);
	}
      }
      else if (strncmp(argv[i], "-", 1)==0){
	if (!argv[i][1]) continue;
	char last=argv[i][1];
	for (int j=2; argv[i][j]; j++){
	  last=argv[i][j];
	  if (lookup.count(argv[i][j-1])==0){
	    errors.push_back(std::string("undefined short option: -")+argv[i][j-1]);
	    continue;
	  }
	  if (lookup[argv[i][j-1]]==""){
	    errors.push_back(std::string("ambiguous short option: -")+argv[i][j-1]);
	    continue;
	  }
	  set_option(lookup[argv[i][j-1]]);
	}

	if (lookup.count(last)==0){
	  errors.push_back(std::string("undefined short option: -")+last);
	  continue;
	}
	if (lookup[last]==""){
	  errors.push_back(std::string("ambiguous short option: -")+last);
	  continue;
	}

	if (i+1>=argc || strncmp(argv[i+1], "-", 1)==0 ||
	    !options[lookup[last]]->has_value()){
	  set_option(lookup[last]);
	  continue;
	}
	set_option(lookup[last], argv[i+1]);
	i++;
      }
      else{
	others.push_back(argv[i]);
      }
    }

    for (std::map<std::string, option_base*>::iterator p=options.begin();
	 p!=options.end(); p++)
      if (!p->second->valid())
	errors.push_back("need option: --"+std::string(p->first));

    return errors.size()==0;
  }

  std::string error() const{
    std::ostringstream oss;
    for (size_t i=0; i<errors.size(); i++)
      oss<<errors[i]<<std::endl;
    return oss.str();
  }

  std::string usage() const {
    std::ostringstream oss;
    oss<<"usage: "<<prog_name<<" [options] ... "<<ftr<<std::endl;
    oss<<"options:"<<std::endl;

    size_t max_width=0;
    for (size_t i=0; i<ordered.size(); i++){
      max_width=std::max(max_width, ordered[i]->name().length());
    }
    for (size_t i=0; i<ordered.size(); i++){
      if (ordered[i]->short_name()){
	oss<<"  -"<<ordered[i]->short_name()<<", ";
      }
      else{
	oss<<"      ";
      }

      oss<<"--"<<ordered[i]->name();
      for (size_t j=ordered[i]->name().length(); j<max_width+4; j++)
	oss<<' ';
      oss<<ordered[i]->description()<<std::endl;
    }
    return oss.str();
  }

private:

  void set_option(const std::string &name){
    if (options.count(name)==0){
      errors.push_back("undefined option: --"+name);
      return;
    }
    if (!options[name]->set()){
      errors.push_back("option needs value: --"+name);
      return;
    }
  }

  void set_option(const std::string &name, const std::string &value){
    if (options.count(name)==0){
      errors.push_back("undefined option: --"+name);
      return;
    }
    if (!options[name]->set(value)){
      errors.push_back("option value is invalid: --"+name+"="+value);
      return;
    }
  }

  class option_base{
  public:
    virtual ~option_base(){}

    virtual bool has_value() const=0;
    virtual bool set()=0;
    virtual bool set(const std::string &value)=0;
    virtual bool has_set() const=0;
    virtual bool valid() const=0;

    virtual const std::string &name() const=0;
    virtual char short_name() const=0;
    virtual const std::string &description() const=0;
  };

  class option_without_value : public option_base {
  public:
    option_without_value(const std::string &name,
			 char short_name,
			 const std::string &desc)
      :nam(name), snam(short_name), desc(desc), has(false){
    }
    ~option_without_value(){}

    bool has_value() const { return false; }

    bool set(){
      has=true;
      return true;
    }

    bool set(const std::string &){
      return false;
    }

    bool has_set() const {
      return has;
    }

    bool valid() const{
      return true;
    }

    const std::string &name() const{
      return nam;
    }

    char short_name() const{
      return snam;
    }

    const std::string &description() const {
      return desc;
    }

  private:
    std::string nam;
    char snam;
    std::string desc;
    bool has;
  };

  template <class T>
  class option_with_value : public option_base {
  public:
    option_with_value(const std::string &name,
		      char short_name,
		      bool need,
		      const T &def,
		      const std::string &desc)
      : nam(name), snam(short_name), need(need), has(false)
      , def(def), actual(def) {
      this->desc=full_description(desc);
    }
    ~option_with_value(){}

    const T &get() const {
      return actual;
    }

    bool has_value() const { return true; }

    bool set(){
      return false;
    }

    bool set(const std::string &value){
      try{
	actual=read(value);
	has=true;
      }
      catch(const std::exception &e){
	return false;
      }
      return true;
    }

    bool has_set() const{
      return has;
    }

    bool valid() const{
      if (need && !has) return false;
      return true;
    }

    const std::string &name() const{
      return nam;
    }

    char short_name() const{
      return snam;
    }

    const std::string &description() const {
      return desc;
    }

  protected:
    std::string full_description(const std::string &desc){
      return
	desc+"("+detail::readable_typename<T>()+
	(need?"":"[="+detail::lexical_cast<std::string>(def)+"]")
	+")";
    }

    virtual T read(const std::string &s)=0;

    std::string nam;
    char snam;
    bool need;
    std::string desc;

    bool has;
    T def;
    T actual;
  };

  template <class T, class F>
  class option_with_value_with_reader : public option_with_value<T> {
  public:
    option_with_value_with_reader(const std::string &name,
				  char short_name,
				  bool need,
				  const T def,
				  const std::string &desc,
				  F reader)
      : option_with_value<T>(name, short_name, need, def, desc), reader(reader){
    }

  private:
    T read(const std::string &s){
      return reader(s);
    }

    F reader;
  };

  std::map<std::string, option_base*> options;
  std::vector<option_base*> ordered;
  std::string ftr;

  std::string prog_name;
  std::vector<std::string> others;

  std::vector<std::string> errors;
};

} // cmdline
