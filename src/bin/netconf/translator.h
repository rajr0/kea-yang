// Copyright (C) 2018 Internet Systems Consortium, Inc. ("ISC")
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef TRANSLATOR_H
#define TRANSLATOR_H

#include <config.h>

#include <boost/shared_ptr.hpp>
#include <cc/data.h>
#include <netconf_connection.h>
// TODO: move UnixControlClient to a path outside testutils.
#include <src/lib/testutils/unix_control_client.h>

namespace isc {
namespace netconf {

/// @brief This represents a base class for all translators
///
/// Translator is an object that receives callback notification
/// from sysrepo (in YANG format) and converts it to appropriate
/// JSON that can be sent over control channel and understood by Kea
class Translator {
public:
    // Constructor (requires xpath to install a callback)
    Translator(NetconfConnection &connection, const std::string &xpath);

    virtual ~Translator();

    virtual std::string getXPath();

    // This method will be called when the callback returns.
    // Need to figure out the type used.
    void setYangData(void *data);

    // This method translates Netconf data to JSON format
    // understood by Kea.
    virtual void translate() = 0;

    // Once setYangData is called,
    isc::data::ElementPtr getJSON();

protected:
    std::string xpath_;

    void *netconf_data_;

    isc::data::ElementPtr json_;

    NetconfConnection &connection_;

    isc::dhcp::test::UnixControlClient keaCtrlChannel_;
};

typedef boost::shared_ptr<Translator> TranslatorPtr;

}  // namespace netconf
}  // namespace isc

#endif /* TRANSLATOR_H */
